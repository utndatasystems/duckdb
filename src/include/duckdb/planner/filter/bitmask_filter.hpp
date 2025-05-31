// src/include/duckdb/planner/filter/bitmask_filter.hpp
#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/enums/expression_type.hpp"

namespace duckdb {

class BitmaskEqualsFilter : public TableFilter {
public:
	static constexpr const TableFilterType TYPE = TableFilterType::BITMASK_EQUALS;

	Value mask;
	Value expected;

	BitmaskEqualsFilter(Value mask, Value expected);
	~BitmaskEqualsFilter() override;

	FilterPropagateResult CheckStatistics(BaseStatistics &stats) const override;
	unique_ptr<Expression> ToExpression(const Expression &column) const override;
	string ToString(const string &column_name) const override;
	bool Equals(const TableFilter &other) const override;
	unique_ptr<TableFilter> Copy() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableFilter> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb