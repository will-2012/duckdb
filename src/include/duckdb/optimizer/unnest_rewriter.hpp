//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/unnest_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;

//! The UnnestRewriter optimizer traverses the logical operator tree and rewrites duplicate
//! eliminated joins that contain UNNESTs by moving the UNNESTs into the projection of
//! the SELECT
class UnnestRewriter {
public:
	UnnestRewriter() {
	}
	//! Rewrite duplicate eliminated joins with UNNESTs
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	//! Find Delim Joins that contain an UNNEST
	void FindCandidates(unique_ptr<LogicalOperator> *op_ptr, vector<unique_ptr<LogicalOperator> *> &candidates);
	//! Update the column bindings in the topmost LOGICAL_PROJECTION
	void UpdateColumnBindings(idx_t &old_table_index, idx_t &new_table_index, unique_ptr<Expression> &expr);
	//! Rewrite all Delim Joins that contain an UNNEST
	void RewriteCandidate(unique_ptr<LogicalOperator> *candidate);
};

} // namespace duckdb
