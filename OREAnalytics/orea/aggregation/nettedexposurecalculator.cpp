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

#include <orea/aggregation/nettedexposurecalculator.hpp>

#include <ql/time/date.hpp>
#include <ql/time/calendars/weekendsonly.hpp>

using namespace std;
using namespace QuantLib;

namespace ore {
namespace analytics {

NettedExposureCalculator::NettedExposureCalculator(
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
    const bool fullInitialCollateralisation)
    : portfolio_(portfolio), market_(market), cube_(cube),
      baseCurrency_(baseCurrency), configuration_(configuration),
      quantile_(quantile), calcType_(calcType),
      multiPath_(multiPath), nettingSetManager_(nettingSetManager),
      nettingSetValue_(nettingSetValue),
      scenarioData_(scenarioData), cubeInterpretation_(cubeInterpretation),
      applyInitialMargin_(applyInitialMargin), dimCalculator_(dimCalculator),
      fullInitialCollateralisation_(fullInitialCollateralisation) {

    vector<string> nettingSetIds;
    for(auto nettingSet : nettingSetValue)
        nettingSetIds.push_back(nettingSet.first);

    exposureCube_ = boost::make_shared<SinglePrecisionInMemoryCubeN>(
        market_->asofDate(), nettingSetIds, cube->dates(),
        multiPath ? cube->samples() : 1, 2); // EPE, ENE
};

void NettedExposureCalculator::build() {
    LOG("Compute netting set exposure profiles");

    const Date today = market_->asofDate();
    const DayCounter dc = ActualActual();

    vector<Real> times = vector<Real>(cube_->dates().size(), 0.0);
    for (Size i = 0; i < cube_->dates().size(); i++)
        times[i] = dc.yearFraction(today, cube_->dates()[i]);
    
    map<string, Real> nettingSetValueToday;
    map<string, Date> nettingSetMaturity;
    for (Size i = 0; i < portfolio_->trades().size(); ++i) {
        const auto& trade = portfolio_->trades()[i];
        string tradeId = trade->id();
        string nettingSetId = trade->envelope().nettingSetId();
        Real npv = cube_->getT0(i);

        if (nettingSetValueToday.find(nettingSetId) == nettingSetValueToday.end()) {
            nettingSetValueToday[nettingSetId] = 0.0;
            nettingSetMaturity[nettingSetId] = today;
        }

        nettingSetValueToday[nettingSetId] += npv;

        if (trade->maturity() > nettingSetMaturity[nettingSetId])
            nettingSetMaturity[nettingSetId] = trade->maturity();
    }

    Size nettingSetCount = 0;
    for (auto n : nettingSetValue_) {
        string nettingSetId = n.first;

        LOG("Aggregate exposure for netting set " << nettingSetId);
        vector<vector<Real>> data = n.second;

        // Get the collateral account balance paths for the netting set.
        // The pointer may remain empty if there is no CSA or if it is inactive.
        boost::shared_ptr<vector<boost::shared_ptr<CollateralAccount>>> collateral =
            collateralPaths(nettingSetId,
                            nettingSetValueToday[nettingSetId],
                            nettingSetValue_[nettingSetId],
                            nettingSetMaturity[nettingSetId]);

        // Get the CSA index for Eonia Floor calculation below
        colva_[nettingSetId] = 0.0;
        collateralFloor_[nettingSetId] = 0.0;
        boost::shared_ptr<NettingSetDefinition> netting = nettingSetManager_->get(nettingSetId);
        string csaIndexName;
        Handle<IborIndex> csaIndex;
        if (netting->activeCsaFlag()) {
            csaIndexName = netting->index();
            if (csaIndexName != "") {
                csaIndex = market_->iborIndex(csaIndexName);
                QL_REQUIRE(scenarioData_->has(AggregationScenarioDataType::IndexFixing, csaIndexName),
                           "scenario data does not provide index values for " << csaIndexName);
            }
        }

        Handle<YieldTermStructure> curve = market_->discountCurve(baseCurrency_, configuration_);
        vector<Real> epe(cube_->dates().size() + 1, 0.0);
        vector<Real> ene(cube_->dates().size() + 1, 0.0);
        vector<Real> ee_b(cube_->dates().size() + 1, 0.0);
        vector<Real> eee_b(cube_->dates().size() + 1, 0.0);
        vector<Real> eee_b_kva_1(cube_->dates().size() + 1, 0.0);
        vector<Real> eee_b_kva_2(cube_->dates().size() + 1, 0.0);
        vector<Real> eepe_b_kva_1(cube_->dates().size() + 1, 0.0);
        vector<Real> eepe_b_kva_2(cube_->dates().size() + 1, 0.0);
        vector<Real> eab(cube_->dates().size() + 1, 0.0);
        vector<Real> pfe(cube_->dates().size() + 1, 0.0);
        vector<Real> colvaInc(cube_->dates().size() + 1, 0.0);
        vector<Real> eoniaFloorInc(cube_->dates().size() + 1, 0.0);
        Real npv = nettingSetValueToday[nettingSetId];
        if ((fullInitialCollateralisation_) & (netting->activeCsaFlag())) {
            // This assumes that the collateral at t=0 is the same as the npv at t=0.
            epe[0] = 0;
            ene[0] = 0;
            pfe[0] = 0;
        } else {
            epe[0] = std::max(npv, 0.0);
            ene[0] = std::max(-npv, 0.0);
            pfe[0] = std::max(npv, 0.0);
        }
        // The fullInitialCollateralisation flag doesn't affect the eab, which feeds into the "ExpectedCollateral"
        // column of the 'exposure_nettingset_*' reports.  We always assume the full collateral here.
        eab[0] = -npv;
        ee_b[0] = epe[0];
        eee_b[0] = ee_b[0];
        exposureCube_->setT0(epe[0], nettingSetCount, 0);
        exposureCube_->setT0(ene[0], nettingSetCount, 1);

        for (Size j = 0; j < cube_->dates().size(); ++j) {

            Date date = cube_->dates()[j];
            Date prevDate = j > 0 ? cube_->dates()[j - 1] : today;

            vector<Real> distribution(cube_->samples(), 0.0);
            for (Size k = 0; k < cube_->samples(); ++k) {
                Real balance = 0.0;
                if (collateral)
                    balance = collateral->at(k)->accountBalance(date);

                eab[j + 1] += balance / cube_->samples();
                Real exposure = data[j][k] - balance;
                Real dim = 0.0;
                if (applyInitialMargin_) {
                    // Initial Margin
                    // Use IM to reduce exposure
                    // Size dimIndex = j == 0 ? 0 : j - 1;
                    Size dimIndex = j;
                    // dim = nettingSetDIM_[nettingSetId][dimIndex][k];
                    dim = dimCalculator_->dynamicIM(nettingSetId)[dimIndex][k];
                    QL_REQUIRE(dim >= 0, "negative DIM for set " << nettingSetId << ", date " << j << ", sample " << k
                                                                 << ": " << dim);
                }
                epe[j + 1] += std::max(exposure - dim, 0.0) /
                              cube_->samples(); // dim here represents the held IM, and is expressed as a positive number
                ene[j + 1] += std::max(-exposure - dim, 0.0) /
                              cube_->samples(); // dim here represents the posted IM, and is expressed as a positive number
                distribution[k] = exposure;
                if (multiPath_) {
                    exposureCube_->set(std::max(exposure - dim, 0.0), nettingSetCount, j, k, 0);
                    exposureCube_->set(std::max(-exposure - dim, 0.0), nettingSetCount, j, k, 1);
                }

                if (netting->activeCsaFlag()) {
                    Real indexValue = 0.0;
                    DayCounter dc = ActualActual();
                    if (csaIndexName != "") {
                        indexValue = scenarioData_->get(j, k, AggregationScenarioDataType::IndexFixing, csaIndexName);
                        dc = csaIndex->dayCounter();
                    }
                    Real dcf = dc.yearFraction(prevDate, date);
                    Real collateralSpread = (balance >= 0.0 ? netting->collatSpreadRcv() : netting->collatSpreadPay());
                    Real colvaDelta = -balance * collateralSpread * dcf / cube_->samples();
                    // inutuitive floorDelta including collateralSpread would be:
                    // -balance * (max(indexValue - collateralSpread,0) - (indexValue - collateralSpread)) * dcf /
                    // samples
                    Real floorDelta = -balance * std::max(-(indexValue - collateralSpread), 0.0) * dcf / cube_->samples();
                    colvaInc[j + 1] += colvaDelta;
                    colva_[nettingSetId] += colvaDelta;
                    eoniaFloorInc[j + 1] += floorDelta;
                    collateralFloor_[nettingSetId] += floorDelta;
                }
            }
            if (!multiPath_) {
                exposureCube_->set(epe[j + 1], nettingSetCount, j, 0, 0);
                exposureCube_->set(ene[j + 1], nettingSetCount, j, 0, 1);
            }
            ee_b[j + 1] = epe[j + 1] / curve->discount(cube_->dates()[j]);
            eee_b[j + 1] = std::max(eee_b[j], ee_b[j + 1]);
            std::sort(distribution.begin(), distribution.end());
            Size index = Size(floor(quantile_ * (cube_->samples() - 1) + 0.5));
            pfe[j + 1] = std::max(distribution[index], 0.0);
        }
        ee_b_[nettingSetId] = ee_b;
        eee_b_[nettingSetId] = eee_b;
        pfe_[nettingSetId] = pfe;
        expectedCollateral_[nettingSetId] = eab;
        colvaInc_[nettingSetId] = colvaInc;
        eoniaFloorInc_[nettingSetId] = eoniaFloorInc;

        nettingSetCount++;

        Real epe_b = 0;
        Real eepe_b = 0;

        Size t = 0;
        Calendar cal = WeekendsOnly();
        Date maturity = std::min(cal.adjust(today + 1 * Years + 4 * Days), nettingSetMaturity[nettingSetId]);
        QuantLib::Real maturityTime = dc.yearFraction(today, maturity);

        while (t < cube_->dates().size() && times[t] <= maturityTime)
            ++t;

        if (t > 0) {
            vector<double> weights(t);
            weights[0] = times[0];
            for (Size k = 1; k < t; k++)
                weights[k] = times[k] - times[k - 1];
            double totalWeights = std::accumulate(weights.begin(), weights.end(), 0.0);
            for (Size k = 0; k < t; k++)
                weights[k] /= totalWeights;

            for (Size k = 0; k < t; k++) {
                epe_b += ee_b[k] * weights[k];
                eepe_b += eee_b[k] * weights[k];
            }
        }
        epe_b_[nettingSetId] = epe_b;
        eepe_b_[nettingSetId] = eepe_b;
    }
}

boost::shared_ptr<vector<boost::shared_ptr<CollateralAccount>>>
NettedExposureCalculator::collateralPaths(
    const string& nettingSetId,
    const Real& nettingSetValueToday,
    const vector<vector<Real>>& nettingSetValue,
    const Date& nettingSetMaturity) {

    boost::shared_ptr<vector<boost::shared_ptr<CollateralAccount>>> collateral;

    if (!nettingSetManager_->has(nettingSetId) || !nettingSetManager_->get(nettingSetId)->activeCsaFlag()) {
        LOG("CSA missing or inactive for netting set " << nettingSetId);
        return collateral;
    }

    LOG("Build collateral account balance paths for netting set " << nettingSetId);
    boost::shared_ptr<NettingSetDefinition> netting = nettingSetManager_->get(nettingSetId);
    string csaFxPair = netting->csaCurrency() + baseCurrency_;
    Real csaFxRateToday = 1.0;
    if (netting->csaCurrency() != baseCurrency_)
        csaFxRateToday = market_->fxSpot(csaFxPair, configuration_)->value();
    LOG("CSA FX rate for pair " << csaFxPair << " = " << csaFxRateToday);

    // Don't use Settings::instance().evaluationDate() here, this has moved to simulation end date.
    Date today = market_->asofDate();
    string csaIndexName = netting->index();
    Real csaRateToday = market_->iborIndex(csaIndexName, configuration_)->fixing(today);
    LOG("CSA compounding rate for index " << csaIndexName << " = " << csaRateToday);

    // Copy scenario data to keep the collateral exposure helper unchanged
    vector<vector<Real>> csaScenFxRates(cube_->dates().size(), vector<Real>(cube_->samples(), 0.0));
    vector<vector<Real>> csaScenRates(cube_->dates().size(), vector<Real>(cube_->samples(), 0.0));
    if (netting->csaCurrency() != baseCurrency_) {
        QL_REQUIRE(scenarioData_->has(AggregationScenarioDataType::FXSpot, netting->csaCurrency()),
                   "scenario data does not provide FX rates for " << csaFxPair);
    }
    if (csaIndexName != "") {
        QL_REQUIRE(scenarioData_->has(AggregationScenarioDataType::IndexFixing, csaIndexName),
                   "scenario data does not provide index values for " << csaIndexName);
    }
    for (Size j = 0; j < cube_->dates().size(); ++j) {
        for (Size k = 0; k < cube_->samples(); ++k) {
            if (netting->csaCurrency() != baseCurrency_)
                csaScenFxRates[j][k] = cubeInterpretation_->getDefaultAggrionScenarioData(
                    scenarioData_, AggregationScenarioDataType::FXSpot, j, k, csaFxPair);
            else
                csaScenFxRates[j][k] = 1.0;
            if (csaIndexName != "") {
                csaScenRates[j][k] = cubeInterpretation_->getDefaultAggrionScenarioData(
                    scenarioData_, AggregationScenarioDataType::IndexFixing, j, k, csaIndexName);
            }
        }
    }

    collateral = CollateralExposureHelper::collateralBalancePaths(
        netting,              // this netting set's definition
        nettingSetValueToday, // today's netting set NPV
        market_->asofDate(),  // original evaluation date
        nettingSetValue,      // matrix of netting set values by date and sample
        nettingSetMaturity,   // netting set's maximum maturity date
        cube_->dates(),               // vector of future evaluation dates
        csaFxRateToday,       // today's FX rate for CSA to base currency, possibly 1
        csaScenFxRates,       // matrix of fx rates by date and sample, possibly 1
        csaRateToday,         // today's collateral compounding rate in CSA currency
        csaScenRates,         // matrix of CSA ccy short rates by date and sample
        calcType_);
    LOG("Collateral account balance paths for netting set " << nettingSetId << " done");

    return collateral;
}

vector<Real> NettedExposureCalculator::getMeanExposure(const string& tid, Size index) {
    vector<Real> exp(cube_->dates().size(), 0.0);
    for (Size i = 0; i < cube_->dates().size(); i++) {
        for (Size k = 0; k < exposureCube_->samples(); k++) {
            exp[i] += exposureCube_->get(tid, cube_->dates()[i], k, index);
        }
        exp[i] /= exposureCube_->samples();
    }
    return exp;
}

} // namespace analytics
} // namespace ore
