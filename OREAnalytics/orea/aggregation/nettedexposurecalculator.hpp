/*
 Copyright (C) 2020 Quaternion Risk Management Ltd
 All rights reserved.

 This file is part of ORE, a free-software/open-source library
 for transparent pricing and risk analysis - http://opensourcerisk.org

 ORE is free software: you can redistribute it and/or modify it
 under the terms of the Modified BSD License.  You should have received a
 copy of the license along with this program.
 The license is also available online at <http://opensourcerisk.org>

 This program is distributed on the basis that it will form a useful
 contribution to risk analytics and model standardisation, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE. See the license for more details.
*/

/*! \file orea/aggregation/exposure.hpp
    \brief Exposure calculator
    \ingroup analytics
*/

#pragma once

#include <orea/aggregation/dimcalculator.hpp>
#include <orea/aggregation/exposurecalculator.hpp>
#include <orea/aggregation/collatexposurehelper.hpp>

#include <ored/portfolio/nettingsetmanager.hpp>

namespace ore {
namespace analytics {
using namespace QuantLib;
using namespace QuantExt;
using namespace data;
using namespace std;

//! XVA Calculator base class
/*!
  Derived classes implement a constructor with the relavant additional input data
  and a build function that performs the XVA calculations for all netting sets and
  along all paths.
*/
class NettedExposureCalculator {
public:
    NettedExposureCalculator(
        const boost::shared_ptr<Portfolio>& portfolio, const boost::shared_ptr<Market>& market,
        const boost::shared_ptr<NPVCube>& cube,
        const string& baseCurrency, const string& configuration, const Real quantile,
        const CollateralExposureHelper::CalculationType calcType, const bool multiPath,
        const boost::shared_ptr<NettingSetManager>& nettingSetManager,
        const map<string, vector<vector<Real>>>& nettingSetValue,
        const boost::shared_ptr<AggregationScenarioData>& scenarioData,
        const boost::shared_ptr<CubeInterpretation> cubeInterpretation,
        const bool applyInitialMargin,
        const boost::shared_ptr<DynamicInitialMarginCalculator>& dimCalculator,
        const bool fullInitialCollateralisation);

    virtual ~NettedExposureCalculator() {}
    const boost::shared_ptr<NPVCube>& exposureCube() { return exposureCube_; }
    //! Compute exposures along all paths and fill result structurues
    virtual void build();

    vector<Real> epe(const string& nid) { return getMeanExposure(nid, 0); }
    vector<Real> ene(const string& nid) { return getMeanExposure(nid, 1); }
    vector<Real>& ee_b(const string& nid) { return ee_b_[nid]; }
    vector<Real>& eee_b(const string& nid) { return eee_b_[nid]; }
    vector<Real>& pfe(const string& nid) { return pfe_[nid]; }
    vector<Real>& expectedCollateral(const string& nid) { return expectedCollateral_[nid]; }
    vector<Real>& colvaIncrements(const string& nid) { return colvaInc_[nid]; }
    vector<Real>& collateralFloorIncrements(const string& nid) { return eoniaFloorInc_[nid]; }
    Real& epe_b(const string& nid) { return epe_b_[nid]; }
    Real& eepe_b(const string& nid) { return eepe_b_[nid]; }
    Real& colva(const string& nid) { return colva_[nid]; }
    Real& collateralFloor(const string& nid) { return collateralFloor_[nid]; }
    

protected:
    boost::shared_ptr<Portfolio> portfolio_;
    boost::shared_ptr<Market> market_;
    boost::shared_ptr<NPVCube> cube_;
    string baseCurrency_;
    string configuration_;
    Real quantile_;
    CollateralExposureHelper::CalculationType calcType_;
    bool multiPath_;
    const boost::shared_ptr<NettingSetManager> nettingSetManager_;
    map<string, vector<vector<Real>>> nettingSetValue_;
    const boost::shared_ptr<AggregationScenarioData> scenarioData_;
    boost::shared_ptr<CubeInterpretation> cubeInterpretation_;
    const bool applyInitialMargin_;
    const boost::shared_ptr<DynamicInitialMarginCalculator> dimCalculator_;
    const bool fullInitialCollateralisation_;

    // Output
    boost::shared_ptr<NPVCube> exposureCube_;

    map<string, std::vector<Real>> ee_b_;
    map<string, std::vector<Real>> eee_b_;
    map<string, std::vector<Real>> pfe_;
    map<string, std::vector<Real>> expectedCollateral_;
    map<string, std::vector<Real>> colvaInc_;
    map<string, std::vector<Real>> eoniaFloorInc_;
    map<string, Real> epe_b_;
    map<string, Real> eepe_b_;
    map<string, Real> colva_;
    map<string, Real> collateralFloor_;
    vector<Real> getMeanExposure(const string& tid, Size index);

    boost::shared_ptr<vector<boost::shared_ptr<CollateralAccount>>>
    collateralPaths(const string& nettingSetId,
        const Real& nettingSetValueToday,
        const vector<vector<Real>>& nettingSetValue,
        const Date& nettingSetMaturity);
};

} // namespace analytics
} // namespace ore
