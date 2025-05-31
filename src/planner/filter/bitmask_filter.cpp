// src/planner/filter/bitmask_filter.cpp
#include "duckdb/planner/filter/bitmask_filter.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

BitmaskEqualsFilter::BitmaskEqualsFilter(Value mask, Value expected)
    : TableFilter(TableFilterType::BITMASK_EQUALS), mask(std::move(mask)), expected(std::move(expected)) {}

BitmaskEqualsFilter::~BitmaskEqualsFilter() = default;

FilterPropagateResult BitmaskEqualsFilter::CheckStatistics(BaseStatistics &stats) const {
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

string BitmaskEqualsFilter::ToString(const string &column_name) const {
	return "(" + column_name + " & " + mask.ToString() + ") = " + expected.ToString();
}

bool BitmaskEqualsFilter::Equals(const TableFilter &other_p) const {
	if (!TableFilter::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BitmaskEqualsFilter>();
	return mask == other.mask && expected == other.expected;
}

unique_ptr<Expression> BitmaskEqualsFilter::ToExpression(const Expression &column) const {
	auto mask_expr = make_uniq<BoundConstantExpression>(mask);

	// Construct (column & mask)
	auto and_expr = make_uniq<BoundOperatorExpression>(
	    ExpressionType::BITWISE_AND,
	    LogicalType::BIGINT);
	and_expr->children.push_back(column.Copy());
	and_expr->children.push_back(std::move(mask_expr));

	// Construct ((column & mask) = expected)
	return make_uniq<BoundComparisonExpression>(
	    ExpressionType::COMPARE_EQUAL,
	    std::move(and_expr),
	    make_uniq<BoundConstantExpression>(expected));
}

unique_ptr<TableFilter> BitmaskEqualsFilter::Copy() const {
	return make_uniq<BitmaskEqualsFilter>(mask, expected);
}

} // namespace duckdb