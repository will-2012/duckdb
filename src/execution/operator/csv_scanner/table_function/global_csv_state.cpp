#include "duckdb/execution/operator/csv_scanner/global_csv_state.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/execution/operator/csv_scanner/scanner_boundary.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_sniffer.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/main/appender.hpp"

namespace duckdb {

CSVGlobalState::CSVGlobalState(ClientContext &context_p, const shared_ptr<CSVBufferManager> &buffer_manager,
                               const CSVReaderOptions &options, idx_t system_threads_p, const vector<string> &files,
                               vector<column_t> column_ids_p, const ReadCSVData &bind_data_p)
    : context(context_p), system_threads(system_threads_p), column_ids(std::move(column_ids_p)),
      sniffer_mismatch_error(options.sniffer_user_mismatch_error), bind_data(bind_data_p) {

	if (buffer_manager && buffer_manager->GetFilePath() == files[0]) {
		auto state_machine = make_shared<CSVStateMachine>(
		    CSVStateMachineCache::Get(context).Get(options.dialect_options.state_machine_options), options);
		// If we already have a buffer manager, we don't need to reconstruct it to the first file
		file_scans.emplace_back(make_uniq<CSVFileScan>(context, buffer_manager, state_machine, options, bind_data,
		                                               column_ids, file_schema));
	} else {
		// If not we need to construct it for the first file
		file_scans.emplace_back(
		    make_uniq<CSVFileScan>(context, files[0], options, 0, bind_data, column_ids, file_schema));
	};
	//! There are situations where we only support single threaded scanning
	bool many_csv_files = files.size() > 1 && files.size() > system_threads * 2;
	single_threaded = many_csv_files || !options.parallel;
	last_file_idx = 0;
	scanner_idx = 0;
	running_threads = MaxThreads();
	if (single_threaded) {
		current_boundary = CSVIterator();
	} else {
		auto buffer_size = file_scans.back()->buffer_manager->GetBuffer(0)->actual_size;
		current_boundary = CSVIterator(0, 0, 0, 0, buffer_size);
	}
	current_buffer_in_use = make_shared<CSVBufferUsage>(*file_scans.back()->buffer_manager, 0);
}

double CSVGlobalState::GetProgress(const ReadCSVData &bind_data_p) const {
	lock_guard<mutex> parallel_lock(main_mutex);
	idx_t total_files = bind_data.files.size();
	// get the progress WITHIN the current file
	double progress;
	if (file_scans.back()->file_size == 0) {
		progress = 1.0;
	} else {
		// for compressed files, readed bytes may greater than files size.
		progress = std::min(1.0, double(file_scans.back()->bytes_read) / double(file_scans.back()->file_size));
	}
	// now get the total percentage of files read
	double percentage = double(current_boundary.GetFileIdx()) / total_files;
	percentage += (double(1) / double(total_files)) * progress;
	return percentage * 100;
}

unique_ptr<StringValueScanner> CSVGlobalState::Next() {
	if (single_threaded) {
		idx_t cur_idx = last_file_idx++;
		if (cur_idx >= bind_data.files.size()) {
			return nullptr;
		}
		shared_ptr<CSVFileScan> current_file;
		if (cur_idx == 0) {
			current_file = file_scans.back();
		} else {
			current_file = make_shared<CSVFileScan>(context, bind_data.files[cur_idx], bind_data.options, cur_idx,
			                                        bind_data, column_ids, file_schema);
		}
		auto csv_scanner =
		    make_uniq<StringValueScanner>(scanner_idx++, current_file->buffer_manager, current_file->state_machine,
		                                  current_file->error_handler, current_file, false, current_boundary);
		return csv_scanner;
	}
	lock_guard<mutex> parallel_lock(main_mutex);
	if (finished) {
		return nullptr;
	}
	if (current_buffer_in_use->buffer_idx != current_boundary.GetBufferIdx()) {
		current_buffer_in_use =
		    make_shared<CSVBufferUsage>(*file_scans.back()->buffer_manager, current_boundary.GetBufferIdx());
	}
	// We first create the scanner for the current boundary
	auto &current_file = *file_scans.back();
	auto csv_scanner =
	    make_uniq<StringValueScanner>(scanner_idx++, current_file.buffer_manager, current_file.state_machine,
	                                  current_file.error_handler, file_scans.back(), false, current_boundary);

	csv_scanner->buffer_tracker = current_buffer_in_use;

	// We then produce the next boundary
	if (!current_boundary.Next(*current_file.buffer_manager)) {
		// This means we are done scanning the current file
		auto current_file_idx = current_file.file_idx + 1;
		if (current_file_idx < bind_data.files.size()) {
			// If we have a next file we have to construct the file scan for that
			file_scans.emplace_back(make_shared<CSVFileScan>(context, bind_data.files[current_file_idx],
			                                                 bind_data.options, current_file_idx, bind_data, column_ids,
			                                                 file_schema));
			// And re-start the boundary-iterator
			auto buffer_size = file_scans.back()->buffer_manager->GetBuffer(0)->actual_size;
			current_boundary = CSVIterator(current_file_idx, 0, 0, 0, buffer_size);
			current_buffer_in_use = make_shared<CSVBufferUsage>(*file_scans.back()->buffer_manager, 0);
		} else {
			// If not we are done with this CSV Scanning
			finished = true;
		}
	}
	// We initialize the scan
	return csv_scanner;
}

idx_t CSVGlobalState::MaxThreads() const {
	// We initialize max one thread per our set bytes per thread limit
	if (single_threaded) {
		return system_threads;
	}
	idx_t total_threads = file_scans.back()->file_size / CSVIterator::BYTES_PER_THREAD + 1;

	if (total_threads < system_threads) {
		return total_threads;
	}
	return system_threads;
}

void CSVGlobalState::DecrementThread() {
	lock_guard<mutex> parallel_lock(main_mutex);
	D_ASSERT(running_threads > 0);
	running_threads--;
	if (running_threads == 0) {
		FillRejectsTable();
		if (context.client_data->debug_set_max_line_length) {
			context.client_data->debug_max_line_length = file_scans[0]->error_handler->GetMaxLineLength();
		}
	}
}

bool IsCSVErrorAcceptedReject(CSVErrorType type) {
	switch (type) {
	case CSVErrorType::CAST_ERROR:
	case CSVErrorType::TOO_MANY_COLUMNS:
	case CSVErrorType::TOO_FEW_COLUMNS:
	case CSVErrorType::MAXIMUM_LINE_SIZE:
	case CSVErrorType::UNTERMINATED_QUOTES:
	case CSVErrorType::INVALID_UNICODE:
		return true;
	default:
		return false;
	}
}

string CSVErrorTypeToEnum(CSVErrorType type) {
	switch (type) {
	case CSVErrorType::CAST_ERROR:
		return "CAST";
	case CSVErrorType::TOO_FEW_COLUMNS:
		return "MISSING COLUMNS";
	case CSVErrorType::TOO_MANY_COLUMNS:
		return "TOO MANY COLUMNS";
	case CSVErrorType::MAXIMUM_LINE_SIZE:
		return "LINE SIZE OVER MAXIMUM";
	case CSVErrorType::UNTERMINATED_QUOTES:
		return "UNQUOTED VALUE";
	case CSVErrorType::INVALID_UNICODE:
		return "INVALID UNICODE";
	default:
		throw InternalException("CSV Error is not valid to be stored in a Rejects Table");
	}
}

void CSVGlobalState::FillRejectsTable() {
	auto &options = bind_data.options;

	if (options.store_rejects.GetValue()) {
		auto limit = options.rejects_limit;
		auto rejects = CSVRejectsTable::GetOrCreate(context, options.rejects_table_name);
		lock_guard<mutex> lock(rejects->write_lock);
		auto &errors_table = rejects->GetErrorsTable(context);
		auto &scans_table = rejects->GetScansTable(context);
		InternalAppender errors_appender(context, errors_table);
		InternalAppender scans_appender(context, scans_table);
		idx_t scan_id = context.transaction.GetActiveQuery();
		idx_t file_id = 0;
		for (auto &file : file_scans) {
			auto file_name = file->file_path;
			auto &errors = file->error_handler->errors;
			// We first insert the file into the file scans table
			for (auto &error_vector : errors) {
				for (auto &error : error_vector.second) {
					if (!IsCSVErrorAcceptedReject(error.type)) {
						// For now, we only will use it for casting errors
						continue;
					}
					// short circuit if we already have too many rejects
					if (limit == 0 || rejects->count < limit) {
						if (limit != 0 && rejects->count >= limit) {
							break;
						}
						rejects->count++;
						auto row_line = file->error_handler->GetLine(error.error_info);
						auto col_idx = error.column_idx;
						// Add the row to the rejects table
						errors_appender.BeginRow();
						// 1. Scan Id
						errors_appender.Append(scan_id);
						// 2. File Id
						errors_appender.Append(file_id);
						// 3. Row Line
						errors_appender.Append(row_line);
						// 4. Byte Position where error occurred
						errors_appender.Append(error.byte_position);
						// 5. Column Index
						errors_appender.Append(col_idx + 1);
						// 6. Column Name (If Applicable)
						switch (error.type) {
						case CSVErrorType::TOO_MANY_COLUMNS:
							errors_appender.Append(Value());
							break;
						case CSVErrorType::TOO_FEW_COLUMNS:
							D_ASSERT(bind_data.return_names.size() > col_idx + 1);
							errors_appender.Append(string_t("\"" + bind_data.return_names[col_idx + 1] + "\""));
							break;
						default:
							errors_appender.Append(string_t("\"" + bind_data.return_names[col_idx] + "\""));
						}
						// 7. Error Type
						errors_appender.Append(string_t(CSVErrorTypeToEnum(error.type)));
						// 8. Original CSV Line
						errors_appender.Append(string_t(error.csv_row));
						// 9. Full Error Message
						errors_appender.Append(string_t(error.error_message));
						errors_appender.EndRow();
					}
					errors_appender.Close();
				}
			}
		}
	}
}

} // namespace duckdb
