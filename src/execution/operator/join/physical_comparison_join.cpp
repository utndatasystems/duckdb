#include "duckdb/execution/operator/join/physical_comparison_join.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

namespace {

void CollectRelationNames(const LogicalOperator &op, unordered_map<idx_t, string> &relation_names) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto table = get.GetTable();
		relation_names[get.table_index] = table ? table->name : get.GetName();
	}
	for (auto &child : op.children) {
		CollectRelationNames(*child, relation_names);
	}
	auto table_indexes = op.GetTableIndex();
	if (op.children.empty() && table_indexes.size() == 1 && !relation_names.count(table_indexes[0])) {
		relation_names[table_indexes[0]] = op.GetName();
	}
}

string FormatQualifiedExpression(const Expression &expr, const vector<ColumnBinding> &bindings,
                                 const unordered_map<idx_t, string> &relation_names) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF: {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (ref.index >= bindings.size()) {
			return expr.ToString();
		}
		auto binding = bindings[ref.index];
		auto entry = relation_names.find(binding.table_index);
		if (entry == relation_names.end() || expr.GetAlias().empty()) {
			return expr.ToString();
		}
		return entry->second + "." + expr.GetAlias();
	}
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		auto entry = relation_names.find(ref.binding.table_index);
		if (entry == relation_names.end() || expr.GetAlias().empty()) {
			return expr.ToString();
		}
		return entry->second + "." + expr.GetAlias();
	}
	default:
		return expr.ToString();
	}
}

string FormatQualifiedCondition(const JoinCondition &condition, const vector<ColumnBinding> &left_bindings,
                                const vector<ColumnBinding> &right_bindings,
                                const unordered_map<idx_t, string> &relation_names) {
	return StringUtil::Format("%s %s %s", FormatQualifiedExpression(*condition.left, left_bindings, relation_names),
	                          ExpressionTypeToOperator(condition.comparison),
	                          FormatQualifiedExpression(*condition.right, right_bindings, relation_names));
}

} // namespace

PhysicalComparisonJoin::PhysicalComparisonJoin(PhysicalPlan &physical_plan, LogicalOperator &op,
                                               PhysicalOperatorType type, vector<JoinCondition> conditions_p,
                                               JoinType join_type, idx_t estimated_cardinality)
    : PhysicalJoin(physical_plan, op, type, join_type, estimated_cardinality), conditions(std::move(conditions_p)) {
	ReorderConditions(conditions);
	if (op.children.size() == 2) {
		unordered_map<idx_t, string> relation_names;
		CollectRelationNames(*op.children[0], relation_names);
		CollectRelationNames(*op.children[1], relation_names);

		auto left_bindings = op.children[0]->GetColumnBindings();
		auto right_bindings = op.children[1]->GetColumnBindings();
		condition_display_strings.reserve(conditions.size());
		for (auto &condition : conditions) {
			condition_display_strings.push_back(
			    FormatQualifiedCondition(condition, left_bindings, right_bindings, relation_names));
		}
	}
}

InsertionOrderPreservingMap<string> PhysicalComparisonJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Join Type"] = EnumUtil::ToString(join_type);
	string condition_info;
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			condition_info += "\n";
		}
		if (i < condition_display_strings.size()) {
			condition_info += condition_display_strings[i];
		} else {
			auto &join_condition = conditions[i];
			condition_info += StringUtil::Format("%s %s %s", join_condition.left->GetName(),
			                                     ExpressionTypeToOperator(join_condition.comparison),
			                                     join_condition.right->GetName());
		}
	}
	result["Conditions"] = condition_info;
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

void PhysicalComparisonJoin::ReorderConditions(vector<JoinCondition> &conditions) {
	// we reorder conditions so the ones with COMPARE_EQUAL occur first
	// check if this is already the case
	bool is_ordered = true;
	bool seen_non_equal = false;
	for (auto &cond : conditions) {
		if (cond.comparison == ExpressionType::COMPARE_EQUAL ||
		    cond.comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			if (seen_non_equal) {
				is_ordered = false;
				break;
			}
		} else {
			seen_non_equal = true;
		}
	}
	if (is_ordered) {
		// no need to re-order
		return;
	}
	// gather lists of equal/other conditions
	vector<JoinCondition> equal_conditions;
	vector<JoinCondition> other_conditions;
	for (auto &cond : conditions) {
		if (cond.comparison == ExpressionType::COMPARE_EQUAL ||
		    cond.comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			equal_conditions.push_back(std::move(cond));
		} else {
			other_conditions.push_back(std::move(cond));
		}
	}
	conditions.clear();
	// reconstruct the sorted conditions
	for (auto &cond : equal_conditions) {
		conditions.push_back(std::move(cond));
	}
	for (auto &cond : other_conditions) {
		conditions.push_back(std::move(cond));
	}
}

void PhysicalComparisonJoin::ConstructEmptyJoinResult(JoinType join_type, bool has_null, DataChunk &input,
                                                      DataChunk &result) {
	// empty hash table, special case
	if (join_type == JoinType::ANTI) {
		// anti join with empty hash table, NOP join
		// return the input
		D_ASSERT(input.ColumnCount() == result.ColumnCount());
		result.Reference(input);
	} else if (join_type == JoinType::MARK) {
		// MARK join with empty hash table
		D_ASSERT(result.ColumnCount() == input.ColumnCount() + 1);
		auto &result_vector = result.data.back();
		D_ASSERT(result_vector.GetType() == LogicalType::BOOLEAN);
		// for every data vector, we just reference the child chunk
		result.SetCardinality(input);
		for (idx_t i = 0; i < input.ColumnCount(); i++) {
			result.data[i].Reference(input.data[i]);
		}
		// for the MARK vector:
		// if the HT has no NULL values (i.e. empty result set), return a vector that has false for every input
		// entry if the HT has NULL values (i.e. result set had values, but all were NULL), return a vector that
		// has NULL for every input entry
		if (!has_null) {
			auto bool_result = FlatVector::GetData<bool>(result_vector);
			for (idx_t i = 0; i < result.size(); i++) {
				bool_result[i] = false;
			}
		} else {
			FlatVector::Validity(result_vector).SetAllInvalid(result.size());
		}
	} else if (join_type == JoinType::LEFT || join_type == JoinType::OUTER || join_type == JoinType::SINGLE) {
		// LEFT/FULL OUTER/SINGLE join and build side is empty
		// for the LHS we reference the data
		result.SetCardinality(input.size());
		for (idx_t i = 0; i < input.ColumnCount(); i++) {
			result.data[i].Reference(input.data[i]);
		}
		// for the RHS
		for (idx_t k = input.ColumnCount(); k < result.ColumnCount(); k++) {
			result.data[k].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(result.data[k], true);
		}
	}
}

} // namespace duckdb
