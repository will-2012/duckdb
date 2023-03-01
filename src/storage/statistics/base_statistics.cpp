#include "duckdb/common/exception.hpp"
#include "duckdb/common/field_writer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/statistics/distinct_statistics.hpp"
#include "duckdb/storage/statistics/list_statistics.hpp"
#include "duckdb/storage/statistics/numeric_statistics.hpp"
#include "duckdb/storage/statistics/string_statistics.hpp"
#include "duckdb/storage/statistics/struct_statistics.hpp"
#include "duckdb/storage/statistics/validity_statistics.hpp"

namespace duckdb {

BaseStatistics::BaseStatistics(LogicalType type) : distinct_count(0), type(std::move(type)) {
}

BaseStatistics::~BaseStatistics() {
}

void BaseStatistics::InitializeBase() {
	validity_stats = make_unique<ValidityStatistics>(false);
}

bool BaseStatistics::CanHaveNull() const {
	if (!validity_stats) {
		// we don't know
		// solid maybe
		return true;
	}
	return ((ValidityStatistics &)*validity_stats).has_null;
}

bool BaseStatistics::CanHaveNoNull() const {
	if (!validity_stats) {
		// we don't know
		// solid maybe
		return true;
	}
	return ((ValidityStatistics &)*validity_stats).has_no_null;
}

void MergeInternal(unique_ptr<BaseStatistics> &orig, const unique_ptr<BaseStatistics> &other) {
	if (other) {
		if (orig) {
			orig->Merge(*other);
		} else {
			orig = other->Copy();
		}
	}
}

void BaseStatistics::Merge(const BaseStatistics &other) {
	D_ASSERT(type == other.type);
	MergeInternal(validity_stats, other.validity_stats);
}

idx_t BaseStatistics::GetDistinctCount() {
	return distinct_count;
}

unique_ptr<BaseStatistics> BaseStatistics::CreateEmpty(LogicalType type) {
	unique_ptr<BaseStatistics> result;
	switch (type.InternalType()) {
	case PhysicalType::BIT:
		return make_unique<ValidityStatistics>(false, false);
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		result = make_unique<NumericStatistics>(std::move(type));
		break;
	case PhysicalType::VARCHAR:
		result = make_unique<StringStatistics>(std::move(type));
		break;
	case PhysicalType::STRUCT:
		result = make_unique<StructStatistics>(std::move(type));
		break;
	case PhysicalType::LIST:
		result = make_unique<ListStatistics>(std::move(type));
		break;
	case PhysicalType::INTERVAL:
	default:
		result = make_unique<BaseStatistics>(std::move(type));
	}
	result->InitializeBase();
	return result;
}

unique_ptr<BaseStatistics> BaseStatistics::Copy() const {
	auto result = make_unique<BaseStatistics>(type);
	result->CopyBase(*this);
	return result;
}

void BaseStatistics::CopyBase(const BaseStatistics &orig) {
	if (orig.validity_stats) {
		validity_stats = orig.validity_stats->Copy();
	}
	distinct_count = orig.distinct_count;
}

void BaseStatistics::Serialize(Serializer &serializer) const {
	FieldWriter writer(serializer);
	ValidityStatistics(CanHaveNull(), CanHaveNoNull()).Serialize(writer);
	Serialize(writer);
	writer.Finalize();
}

void BaseStatistics::Serialize(FieldWriter &writer) const {
}

unique_ptr<BaseStatistics> BaseStatistics::Deserialize(Deserializer &source, LogicalType type) {
	FieldReader reader(source);
	auto validity_stats = ValidityStatistics::Deserialize(reader);
	unique_ptr<BaseStatistics> result;
	switch (type.InternalType()) {
	case PhysicalType::BIT:
		result = ValidityStatistics::Deserialize(reader);
		break;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		result = NumericStatistics::Deserialize(reader, std::move(type));
		break;
	case PhysicalType::VARCHAR:
		result = StringStatistics::Deserialize(reader, std::move(type));
		break;
	case PhysicalType::STRUCT:
		result = StructStatistics::Deserialize(reader, std::move(type));
		break;
	case PhysicalType::LIST:
		result = ListStatistics::Deserialize(reader, std::move(type));
		break;
	case PhysicalType::INTERVAL:
		result = make_unique<BaseStatistics>(std::move(type));
		break;
	default:
		throw InternalException("Unimplemented type for statistics deserialization");
	}
	if (type.InternalType() != PhysicalType::BIT) {
		result->validity_stats = std::move(validity_stats);
	}

	reader.Finalize();
	return result;
}

string BaseStatistics::ToString() const {
	return StringUtil::Format("%s%s", validity_stats ? validity_stats->ToString() : "",
	                          distinct_count > 0 ? StringUtil::Format("[Approx Unique: %lld]", distinct_count) : "");
}

void BaseStatistics::Verify(Vector &vector, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(vector.GetType() == this->type);
	if (validity_stats) {
		validity_stats->Verify(vector, sel, count);
	}
}

void BaseStatistics::Verify(Vector &vector, idx_t count) const {
	auto sel = FlatVector::IncrementalSelectionVector();
	Verify(vector, *sel, count);
}

} // namespace duckdb
