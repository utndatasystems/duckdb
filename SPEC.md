Iteration [1]



1. How do you inject cardinalities for queries with 2 table names?
- We can match based on the string representation of the filters.

```
"tag": "12a", "config": ["cn", "ct", "it1", "it2", "mc", "mi", "mi_idx", "t"]

explain select count(*)::bigint from company_name AS cn, company_type AS ct, info_type AS it1, info_type AS it2, movie_companies AS mc, movie_info AS mi, movie_info_idx AS mi_idx, title AS t WHERE cn.country_code = '[us]' AND ct.kind = 'production companies' AND it1.info = 'genres' AND it2.info = 'rating' AND mi.info in ('Drama', 'Horror') AND mi_idx.info > '8.0' AND t.production_year >= 2005 AND t.production_year <= 2008 AND t.id = mi.movie_id AND t.id = mi_idx.movie_id AND mi.info_type_id = it1.id AND mi_idx.info_type_id = it2.id AND t.id = mc.movie_id AND ct.id = mc.company_type_id AND cn.id = mc.company_id AND mc.movie_id = mi.movie_id AND mc.movie_id = mi_idx.movie_id AND mi.movie_id = mi_idx.movie_id;

"act": 397
```

Since my autobound doesn't differentiate here, I really don't get how to handle this:

```

2. Seems it removed this comment from v1.5.3:

```
-// Cardinality is calculatd using logic found in
-// https://blobs.duckdb.org/papers/tom-ebergen-msc-thesis-join-order-optimization-with-almost-no-statistics.pdf TL;DR
-// Cardinality is estimated based on cardinality of base tables and the distinct counts of joined columns. If you have
-// two tables A and B joined using A.x = B.y we assume that each tuple in A will match ~ B/(distinct(y)) tuples in B.
-// The cardinality estimation then becomes (|A|x|B|) / max(distinct(x), distinct(y)).
-// If there are extra joins, you can add the cardinality of the table to the numerator, and the
-// distinct count of the join condition to the denominator.
-// One benefit of this cardinality estimation formula is that it is associative and commutative, which means regardless
-// of the order of the joins/join tree, the cardinality estimate will always be the same. The drawback of this current
-// implementation, however, is that it only considers equality join conditions. Some modification have been made for
-// comparison types like <, <=, >, >=, !=, but only a "penalty" was introduced, and the calculated cardinality is not
-// based on stats (see CalculateUpdatedDenom()).
 template <>
-double CardinalityEstimator::EstimateCardinalityWithSet(JoinRelationSet &new_set) {
+double CardinalityEstimator::EstimateCardinalityWithSet(JoinRelationSet &new_set, QueryGraphManager& query_graph_manager) {
+       auto getTableNames = [&query_graph_manager]() {
+               std::vector<std::string> table_names;
+               for (auto& elem : query_graph_manager.relation_manager.GetRelationStats()) {
+                       table_names.push_back(elem.table_name);
+               }
+               return table_names;
+       };
+
```