#include <dplyr/main.h>

#include <boost/scoped_ptr.hpp>

#include <tools/LazyDots.h>

#include <dplyr/check_supported_type.h>

#include <dplyr/GroupedDataFrame.h>
#include <dplyr/RowwiseDataFrame.h>
#include <dplyr/tbl_cpp.h>

#include <dplyr/Result/LazyRowwiseSubsets.h>
#include <dplyr/Result/CallProxy.h>

#include <dplyr/Gatherer.h>
#include <dplyr/Replicator.h>
#include <dplyr/NamedListAccumulator.h>

using namespace Rcpp;
using namespace dplyr;

template <typename Data>
SEXP structure_mutate(const NamedListAccumulator<Data>& accumulator, const DataFrame& df, CharacterVector classes) {
  List res = accumulator;
  res.attr("class") = classes;
  set_rownames(res, df.nrows());
  res.attr("vars")   = df.attr("vars");
  res.attr("labels")  = df.attr("labels");
  res.attr("index")  = df.attr("index");
  res.attr("indices") = df.attr("indices");
  res.attr("drop") = df.attr("drop");
  res.attr("group_sizes") = df.attr("group_sizes");
  res.attr("biggest_group_size") = df.attr("biggest_group_size");

  return res;
}

void check_not_groups(const LazyDots& dots, const RowwiseDataFrame& gdf) {}

void check_not_groups(const LazyDots& dots, const GroupedDataFrame& gdf) {
  int n = dots.size();
  for (int i=0; i<n; i++) {
    if (gdf.has_group(dots[i].name()))
      stop("cannot modify grouping variable");
  }
}


SEXP mutate_not_grouped(DataFrame df, const LazyDots& dots) {
  int nexpr = dots.size();
  int nrows = df.nrows();

  NamedListAccumulator<DataFrame> accumulator;
  int nvars = df.size();
  if (nvars) {
    CharacterVector df_names = df.names();
    for (int i=0; i<nvars; i++) {
      accumulator.set(Symbol(df_names[i]), df[i]);
    }
  }

  CallProxy call_proxy(df);
  List results(nexpr);

  for (int i=0; i<nexpr; i++) {
    Rcpp::checkUserInterrupt();
    const Lazy& lazy = dots[i];

    Shield<SEXP> call_(lazy.expr());
    SEXP call = call_;
    Symbol name = lazy.name();
    Environment env = lazy.env();
    call_proxy.set_env(env);

    if (TYPEOF(call) == SYMSXP) {
      if (call_proxy.has_variable(call)) {
        results[i] = call_proxy.get_variable(PRINTNAME(call));
      } else {
        results[i] = shared_SEXP(env.find(CHAR(PRINTNAME(call))));
      }
    } else if (TYPEOF(call) == LANGSXP) {
      call_proxy.set_call(call);
      results[i] = call_proxy.eval();
    } else if (Rf_length(call) == 1) {
      boost::scoped_ptr<Gatherer> gather(constant_gatherer(call, nrows));
      results[i] = gather->collect();
    } else if (Rf_isNull(call)) {
      accumulator.rm(name);
      continue;
    } else {
      stop("cannot handle");
    }

    check_supported_type(results[i], name.c_str());

    if (Rf_inherits(results[i], "POSIXlt")) {
      stop("`mutate` does not support `POSIXlt` results");
    }
    int n_res = Rf_length(results[i]);
    if (n_res == nrows) {
      // ok
    } else if (n_res == 1) {
      // recycle
      boost::scoped_ptr<Gatherer> gather(constant_gatherer(results[i] , df.nrows()));
      results[i] = gather->collect();
    } else {
      stop("wrong result size (%d), expected %d or 1", n_res, nrows);
    }

    call_proxy.input(name, results[i]);
    accumulator.set(name, results[i]);
  }
  List res = structure_mutate(accumulator, df, classes_not_grouped());

  return res;
}

template <typename Data, typename Subsets>
SEXP mutate_grouped(const DataFrame& df, const LazyDots& dots) {
  LOG_VERBOSE << "checking zero rows";

  // special 0 rows case
  if (df.nrows() == 0) {
    DataFrame res = mutate_not_grouped(df, dots);
    res.attr("vars") = df.attr("vars");
    res.attr("class") = df.attr("class");
    return Data(res).data();
  }

  LOG_VERBOSE << "initializing proxy";

  typedef GroupedCallProxy<Data, Subsets> Proxy;
  Data gdf(df);
  int nexpr = dots.size();
  check_not_groups(dots, gdf);

  Proxy proxy(gdf);

  LOG_VERBOSE << "copying data to accumulator";

  NamedListAccumulator<Data> accumulator;
  int ncolumns = df.size();
  CharacterVector column_names = df.names();
  for (int i=0; i<ncolumns; i++) {
    accumulator.set(Symbol(column_names[i]), df[i]);
  }

  LOG_VERBOSE << "processing " << nexpr << " variables";

  List variables(nexpr);
  for (int i=0; i<nexpr; i++) {
    Rcpp::checkUserInterrupt();
    const Lazy& lazy = dots[i];

    Environment env = lazy.env();
    Shield<SEXP> call_(lazy.expr());
    SEXP call = call_;
    Symbol name = lazy.name();
    proxy.set_env(env);

    LOG_VERBOSE << "processing " << CharacterVector(name);

    if (TYPEOF(call) == LANGSXP || TYPEOF(call) == SYMSXP) {
      proxy.set_call(call);
      boost::scoped_ptr<Gatherer> gather(gatherer<Data, Subsets>(proxy, gdf, name));
      SEXP variable = variables[i] = gather->collect();
      proxy.input(name, variable);
      accumulator.set(name, variable);
    } else if (Rf_length(call) == 1) {
      boost::scoped_ptr<Gatherer> gather(constant_gatherer(call, gdf.nrows()));
      SEXP variable = variables[i] = gather->collect();
      proxy.input(name, variable);
      accumulator.set(name, variable);
    } else if (Rf_isNull(call)) {
      accumulator.rm(name);
      continue;
    } else {
      stop("cannot handle");
    }
  }

  return structure_mutate(accumulator, df, df.attr("class"));
}


// [[Rcpp::export]]
SEXP mutate_impl(DataFrame df, LazyDots dots) {
  if (dots.size() == 0) return df;
  check_valid_colnames(df);
  if (is<RowwiseDataFrame>(df)) {
    return mutate_grouped<RowwiseDataFrame, LazyRowwiseSubsets>(df, dots);
  } else if (is<GroupedDataFrame>(df)) {
    return mutate_grouped<GroupedDataFrame, LazyGroupedSubsets>(df, dots);
  } else {
    return mutate_not_grouped(df, dots);
  }
}
