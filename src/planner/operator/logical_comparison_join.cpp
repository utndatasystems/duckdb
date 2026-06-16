#include "duckdb/planner/operator/logical_comparison_join.hpp"
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
		relation_names[get.table_index] = !get.relation_name.empty() ? get.relation_name : get.GetName();
	}
	for (auto &child : op.children) {
		CollectRelationNames(*child, relation_names);
	}
	auto table_indexes = op.GetTableIndex();
	if (op.children.empty() && table_indexes.size() == 1 && !relation_names.count(table_indexes[0])) {
		relation_names[table_indexes[0]] = op.GetName();
	}
}

vector<string> GetOutputNames(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		vector<string> result;
		if (get.projection_ids.empty()) {
			for (auto &column_id : get.GetColumnIds()) {
				result.push_back(get.GetColumnName(column_id));
			}
		} else {
			for (auto projection_id : get.projection_ids) {
				result.push_back(get.names[projection_id]);
			}
		}
		if (!get.projected_input.empty() && !get.children.empty()) {
			auto child_names = GetOutputNames(*get.children[0]);
			for (auto entry : get.projected_input) {
				result.push_back(child_names[entry]);
			}
		}
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		vector<string> result;
		result.reserve(op.expressions.size());
		for (auto &expr : op.expressions) {
			result.push_back(expr->GetName());
		}
		return result;
	}
	vector<ColumnBinding> child_bindings;
	vector<string> child_names;
	for (auto &child : op.children) {
		auto names = GetOutputNames(*child);
		auto bindings = child->GetColumnBindings();
		child_names.insert(child_names.end(), names.begin(), names.end());
		child_bindings.insert(child_bindings.end(), bindings.begin(), bindings.end());
	}
	vector<string> result;
	auto &mutable_op = const_cast<LogicalOperator &>(op);
	for (auto &binding : mutable_op.GetColumnBindings()) {
		auto it = std::find(child_bindings.begin(), child_bindings.end(), binding);
		if (it == child_bindings.end()) {
			return {};
		}
		result.push_back(child_names[it - child_bindings.begin()]);
	}
	return result;
}

string FormatQualifiedExpression(const Expression &expr, const vector<ColumnBinding> &bindings,
                                 const vector<string> &output_names,
                                 const unordered_map<idx_t, string> &relation_names) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF: {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (ref.index >= bindings.size() || ref.index >= output_names.size()) {
			return expr.ToString();
		}
		auto binding = bindings[ref.index];
		auto entry = relation_names.find(binding.table_index);
		if (entry == relation_names.end() || output_names[ref.index].empty()) {
			return expr.ToString();
		}
		return entry->second + "." + output_names[ref.index];
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
                                const vector<string> &left_names, const vector<ColumnBinding> &right_bindings,
                                const vector<string> &right_names,
                                const unordered_map<idx_t, string> &relation_names) {
	return StringUtil::Format("%s %s %s",
	                          FormatQualifiedExpression(*condition.left, left_bindings, left_names, relation_names),
	                          ExpressionTypeToOperator(condition.comparison),
	                          FormatQualifiedExpression(*condition.right, right_bindings, right_names, relation_names));
}

} // namespace

LogicalComparisonJoin::LogicalComparisonJoin(JoinType join_type, LogicalOperatorType logical_type)
    : LogicalJoin(join_type, logical_type) {
}

InsertionOrderPreservingMap<string> LogicalComparisonJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Join Type"] = EnumUtil::ToChars(join_type);

	unordered_map<idx_t, string> relation_names;
	CollectRelationNames(*children[0], relation_names);
	CollectRelationNames(*children[1], relation_names);
	auto left_bindings = children[0]->GetColumnBindings();
	auto left_names = GetOutputNames(*children[0]);
	auto right_bindings = children[1]->GetColumnBindings();
	auto right_names = GetOutputNames(*children[1]);

	string conditions_info;
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			conditions_info += "\n";
		}
		conditions_info += FormatQualifiedCondition(conditions[i], left_bindings, left_names, right_bindings,
		                                           right_names, relation_names);
	}
	if (predicate) {
		if (!conditions.empty()) {
			conditions_info += "\n";
		}
		conditions_info += predicate->ToString();
	}
	result["Conditions"] = conditions_info;
	SetParamsEstimatedCardinality(result);

	return result;
}

bool LogicalComparisonJoin::HasEquality(idx_t &range_count) const {
	bool result = false;
	for (size_t c = 0; c < conditions.size(); ++c) {
		auto &cond = conditions[c];
		switch (cond.comparison) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			result = true;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			++range_count;
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_DISTINCT_FROM:
			break;
		default:
			throw NotImplementedException("Unimplemented comparison join");
		}
	}
	return result;
}

} // namespace duckdb
