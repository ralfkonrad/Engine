/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2015 Quaternion Risk Management

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include <qle/models/fxbspiecewiseconstantparametrization.hpp>

namespace QuantExt {

FxBsPiecewiseConstantParametrization::FxBsPiecewiseConstantParametrization(
    const Currency &currency,
    const Handle<YieldTermStructure> &foreignTermStructure,
    const Handle<Quote> &fxSpotToday, const Array &times, const Array &sigma)
    : FxBsParametrization(currency, foreignTermStructure, fxSpotToday),
      PiecewiseConstantHelper1(times) {
    QL_REQUIRE(times.size() + 1 == sigma.size(),
               "alpha size (" << sigma.size()
                              << ") inconsistent to times size ("
                              << times.size() << ")");

    // store raw parameter values
    for (Size i = 0; i < PiecewiseConstantHelper1::y_.size(); ++i) {
        PiecewiseConstantHelper1::y_.setParam(i, inverse(0, sigma[i]));
    }
    update();
}

} // namespace QuantExt
