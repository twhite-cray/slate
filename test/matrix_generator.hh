// Copyright (c) 2017-2020, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#ifndef SLATE_MATRIX_GENERATOR_HH
#define SLATE_MATRIX_GENERATOR_HH

#include <exception>
#include <complex>
#include <ctype.h>

#include "testsweeper.hh"
#include "blas.hh"
#include "lapack.hh"
#include "slate/slate.hh"

#include "matrix_params.hh"

namespace slate {

// -----------------------------------------------------------------------------
const int64_t idist_rand  = 1;
const int64_t idist_rands = 2;
const int64_t idist_randn = 3;

enum class TestMatrixType {
    rand      = 1,  // maps to larnv idist
    rands     = 2,  // maps to larnv idist
    randn     = 3,  // maps to larnv idist
    zero,
    identity,
    jordan,
    diag,
    svd,
    poev,
    heev,
    geev,
    geevx,
};

enum class TestMatrixDist {
    rand      = 1,  // maps to larnv idist
    rands     = 2,  // maps to larnv idist
    randn     = 3,  // maps to larnv idist
    arith,
    geo,
    cluster0,
    cluster1,
    rarith,
    rgeo,
    rcluster0,
    rcluster1,
    logrand,
    specified,
    none,
};

// -----------------------------------------------------------------------------
template< typename scalar_t >
void generate_matrix(
    MatrixParams& params,
    slate::Matrix< scalar_t >& A,
    std::vector< blas::real_type<scalar_t> >& sigma );

// Overload without sigma.
template< typename scalar_t >
void generate_matrix(
    MatrixParams& params,
    slate::Matrix< scalar_t >& A );

void generate_matrix_usage();

} // namespace slate

#endif // SLATE_MATRIX_GENERATOR_HH
