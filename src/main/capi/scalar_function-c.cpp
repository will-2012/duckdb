#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct CScalarFunctionInfo : public ScalarFunctionInfo {
	~CScalarFunctionInfo() override {
		if (extra_info && delete_callback) {
			delete_callback(extra_info);
		}
		extra_info = nullptr;
		delete_callback = nullptr;
	}

	duckdb_scalar_function_t function = nullptr;
	void *extra_info = nullptr;
	duckdb_delete_callback_t delete_callback = nullptr;
};

struct CScalarFunctionBindData : public FunctionData {
	CScalarFunctionBindData(CScalarFunctionInfo &info) : info(info) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CScalarFunctionBindData>(info);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CScalarFunctionBindData>();
		return info.extra_info == other.info.extra_info && info.function == other.info.function;
	}

	CScalarFunctionInfo &info;
};

unique_ptr<FunctionData> BindCAPIScalarFunction(ClientContext &context, ScalarFunction &bound_function,
                                                vector<unique_ptr<Expression>> &arguments) {
	auto &info = bound_function.function_info->Cast<CScalarFunctionInfo>();
	return make_uniq<CScalarFunctionBindData>(info);
}

void CAPIScalarFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &bind_info = state.expr.Cast<BoundFunctionExpression>().bind_info;
	auto &c_bind_info = bind_info->Cast<CScalarFunctionBindData>();

	auto all_const = input.AllConstant();
	input.Flatten();
	auto c_input = reinterpret_cast<duckdb_data_chunk>(&input);
	auto c_result = reinterpret_cast<duckdb_vector>(&result);
	c_bind_info.info.function(c_bind_info.info.extra_info, c_input, c_result);
	if (all_const) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

} // namespace duckdb

duckdb_scalar_function duckdb_create_scalar_function() {
	auto function = new duckdb::ScalarFunction("", {}, duckdb::LogicalType::INVALID, duckdb::CAPIScalarFunction,
	                                           duckdb::BindCAPIScalarFunction);
	function->function_info = duckdb::make_shared_ptr<duckdb::CScalarFunctionInfo>();
	return function;
}

void duckdb_destroy_scalar_function(duckdb_scalar_function *function) {
	if (function && *function) {
		auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(*function);
		delete scalar_function;
		*function = nullptr;
	}
}

void duckdb_scalar_function_set_name(duckdb_scalar_function function, const char *name) {
	if (!function || !name) {
		return;
	}
	auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(function);
	scalar_function->name = name;
}

void duckdb_scalar_function_add_parameter(duckdb_scalar_function function, duckdb_logical_type type) {
	if (!function || !type) {
		return;
	}
	auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(function);
	auto logical_type = reinterpret_cast<duckdb::LogicalType *>(type);
	scalar_function->arguments.push_back(*logical_type);
}

void duckdb_scalar_function_set_return_type(duckdb_scalar_function function, duckdb_logical_type type) {
	if (!function || !type) {
		return;
	}
	auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(function);
	auto logical_type = reinterpret_cast<duckdb::LogicalType *>(type);
	scalar_function->return_type = *logical_type;
}

void duckdb_scalar_function_set_extra_info(duckdb_scalar_function function, void *extra_info,
                                           duckdb_delete_callback_t destroy) {
	if (!function || !extra_info) {
		return;
	}
	auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(function);
	auto &info = scalar_function->function_info->Cast<duckdb::CScalarFunctionInfo>();
	info.extra_info = extra_info;
	info.delete_callback = destroy;
}

void duckdb_scalar_function_set_function(duckdb_scalar_function function, duckdb_scalar_function_t execute_func) {
	if (!function || !execute_func) {
		return;
	}
	auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(function);
	auto &info = scalar_function->function_info->Cast<duckdb::CScalarFunctionInfo>();
	info.function = execute_func;
}

duckdb_state duckdb_register_scalar_function(duckdb_connection connection, duckdb_scalar_function function) {
	if (!connection || !function) {
		return DuckDBError;
	}
	auto scalar_function = reinterpret_cast<duckdb::ScalarFunction *>(function);
	auto &info = scalar_function->function_info->Cast<duckdb::CScalarFunctionInfo>();
	if (scalar_function->name.empty() || !info.function ||
	    scalar_function->return_type.id() == duckdb::LogicalTypeId::INVALID) {
		return DuckDBError;
	}
	auto con = reinterpret_cast<duckdb::Connection *>(connection);
	con->context->RunFunctionInTransaction([&]() {
		auto &catalog = duckdb::Catalog::GetSystemCatalog(*con->context);
		duckdb::CreateScalarFunctionInfo sf_info(*scalar_function);

		// create the function in the catalog
		catalog.CreateFunction(*con->context, sf_info);
	});
	return DuckDBSuccess;
}
