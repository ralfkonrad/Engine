/*
 Copyright (C) 2016 Quaternion Risk Management Ltd
 All rights reserved.

 This file is part of OpenRiskEngine, a free-software/open-source library
 for transparent pricing and risk analysis - http://openriskengine.org

 OpenRiskEngine is free software: you can redistribute it and/or modify it
 under the terms of the Modified BSD License.  You should have received a
 copy of the license along with this program; if not, please email
 <users@openriskengine.org>. The license is also available online at
 <http://openriskengine.org/license.shtml>.

 This program is distributed on the basis that it will form a useful
 contribution to risk analytics and model standardisation, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE. See the license for more details.
*/


/*! \file irlgm1fstateprocess.hpp
    \brief ir LGM 1f model state process
    \ingroup 
*/

#ifndef quantext_irlgm1f_stateprocess_hpp
#define quantext_irlgm1f_stateprocess_hpp

#include <qle/models/irlgm1fparametrization.hpp>
#include <ql/stochasticprocess.hpp>

using namespace QuantLib;

namespace QuantExt {

class IrLgm1fStateProcess : public StochasticProcess1D {
  public:
    IrLgm1fStateProcess(
        const boost::shared_ptr<IrLgm1fParametrization> &parametrization);
    //! \name StochasticProcess interface
    //@{
    Real x0() const;
    Real drift(Time t, Real x) const;
    Real diffusion(Time t, Real x) const;
    Real expectation(Time t0, Real x0, Time dt) const;
    Real stdDeviation(Time t0, Real x0, Time dt) const;
    Real variance(Time t0, Real x0, Time dt) const;
    //@}
  private:
    const boost::shared_ptr<IrLgm1fParametrization> p_;
};

// inline

inline Real IrLgm1fStateProcess::x0() const { return 0.0; }

inline Real IrLgm1fStateProcess::drift(Time, Real) const { return 0.0; }

inline Real IrLgm1fStateProcess::diffusion(Time t, Real) const {
    return p_->alpha(t);
}

inline Real IrLgm1fStateProcess::expectation(Time, Real x0, Time) const {
    return x0;
}

inline Real IrLgm1fStateProcess::variance(Time t0, Real, Time dt) const {
    return p_->zeta(t0 + dt) - p_->zeta(t0);
}

inline Real IrLgm1fStateProcess::stdDeviation(Time t0, Real x0, Time dt) const {
    return std::sqrt(variance(t0, x0, dt));
}

} // namespace QuantExt

#endif
