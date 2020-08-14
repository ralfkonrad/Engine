/*
 Copyright (C) 2016-2020 Quaternion Risk Management Ltd
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

#include <orea/aggregation/postprocess.hpp>
#include <orea/aggregation/dimcalculator.hpp>
#include <orea/aggregation/dimregressioncalculator.hpp>
#include <orea/aggregation/dynamiccreditxvacalculator.hpp>
#include <orea/aggregation/xvacalculator.hpp>
#include <orea/aggregation/staticcreditxvacalculator.hpp>
#include <ored/utilities/log.hpp>
#include <ored/utilities/vectorutils.hpp>
#include <ql/errors.hpp>
#include <ql/time/calendars/weekendsonly.hpp>
#include <ql/version.hpp>

#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/generallinearleastsquares.hpp>
#include <ql/math/kernelfunctions.hpp>
#include <ql/methods/montecarlo/lsmbasissystem.hpp>
#include <ql/time/daycounters/actualactual.hpp>

#include <qle/math/nadarayawatson.hpp>
#include <qle/math/stabilisedglls.hpp>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/error_of_mean.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/stats.hpp>

using namespace std;
using namespace QuantLib;

using namespace boost::accumulators;

namespace ore {
namespace analytics {

AllocationMethod parseAllocationMethod(const string& s) {
    static map<string, AllocationMethod> m = {
        {"None", AllocationMethod::None},
        {"Marginal", AllocationMethod::Marginal},
        {"RelativeFairValueGross", AllocationMethod::RelativeFairValueGross},
        {"RelativeFairValueNet", AllocationMethod::RelativeFairValueNet},
        {"RelativeXVA", AllocationMethod::RelativeXVA},
    };

    auto it = m.find(s);
    if (it != m.end()) {
        return it->second;
    } else {
        QL_FAIL("AllocationMethod \"" << s << "\" not recognized");
    }
}

std::ostream& operator<<(std::ostream& out, AllocationMethod m) {
    if (m == AllocationMethod::None)
        out << "None";
    else if (m == AllocationMethod::Marginal)
        out << "Marginal";
    else if (m == AllocationMethod::RelativeFairValueGross)
        out << "RelativeFairValueGross";
    else if (m == AllocationMethod::RelativeFairValueNet)
        out << "RelativeFairValueNet";
    else if (m == AllocationMethod::RelativeXVA)
        out << "RelativeXVA";
    else
        QL_FAIL("Allocation method not covered");
    return out;
}

PostProcess::PostProcess(
    const boost::shared_ptr<Portfolio>& portfolio, const boost::shared_ptr<NettingSetManager>& nettingSetManager,
    const boost::shared_ptr<Market>& market, const std::string& configuration, const boost::shared_ptr<NPVCube>& cube,
    const boost::shared_ptr<AggregationScenarioData>& scenarioData, const map<string, bool>& analytics,
    const string& baseCurrency, const string& allocMethod, Real marginalAllocationLimit, Real quantile,
    const string& calculationType, const string& dvaName, const string& fvaBorrowingCurve,
    const string& fvaLendingCurve,const boost::shared_ptr<DynamicInitialMarginCalculator>& dimCalculator,
    const boost::shared_ptr<CubeInterpretation>& cubeInterpretation, bool fullInitialCollateralisation,
    Real kvaCapitalDiscountRate, Real kvaAlpha, Real kvaRegAdjustment, Real kvaCapitalHurdle, Real kvaOurPdFloor,
    Real kvaTheirPdFloor, Real kvaOurCvaRiskWeight, Real kvaTheirCvaRiskWeight, const boost::shared_ptr<NPVCube>& cptyCube)
    : portfolio_(portfolio), nettingSetManager_(nettingSetManager), market_(market), configuration_(configuration),
      cube_(cube), cptyCube_(cptyCube), scenarioData_(scenarioData), analytics_(analytics), baseCurrency_(baseCurrency), quantile_(quantile),
      calcType_(parseCollateralCalculationType(calculationType)), dvaName_(dvaName),
      fvaBorrowingCurve_(fvaBorrowingCurve), fvaLendingCurve_(fvaLendingCurve), dimCalculator_(dimCalculator),
      cubeInterpretation_(cubeInterpretation), fullInitialCollateralisation_(fullInitialCollateralisation),
      kvaCapitalDiscountRate_(kvaCapitalDiscountRate), kvaAlpha_(kvaAlpha), kvaRegAdjustment_(kvaRegAdjustment),
      kvaCapitalHurdle_(kvaCapitalHurdle), kvaOurPdFloor_(kvaOurPdFloor), kvaTheirPdFloor_(kvaTheirPdFloor),
      kvaOurCvaRiskWeight_(kvaOurCvaRiskWeight), kvaTheirCvaRiskWeight_(kvaTheirCvaRiskWeight) {

    // set a default value for the cube interpretation object if it is NULL
    if (!cubeInterpretation_) {
        WLOG("cube interpretation is not set, use regular");
        cubeInterpretation_ = boost::make_shared<RegularCubeInterpretation>();
    }
    boost::shared_ptr<RegularCubeInterpretation> regularCubeInterpretation =
        boost::dynamic_pointer_cast<RegularCubeInterpretation>(cubeInterpretation_);
    bool isRegularCubeStorage = (regularCubeInterpretation != NULL);

    LOG("cube storage is regular: " << isRegularCubeStorage);
    LOG("cube dates: " << cube->dates().size());

    QL_REQUIRE(marginalAllocationLimit > 0.0, "positive allocationLimit expected");

    // check portfolio and cube have the same trade ids, in the same order
    QL_REQUIRE(portfolio->size() == cube_->ids().size(),
               "PostProcess::PostProcess(): portfolio size ("
                   << portfolio->size() << ") does not match cube trade size (" << cube_->ids().size() << ")");
    for (Size i = 0; i < portfolio->size(); ++i) {
        QL_REQUIRE(portfolio->trades()[i]->id() == cube_->ids()[i], "PostProcess::PostProcess(): portfolio trade #"
                                                                        << i << " (id=" << portfolio->trades()[i]->id()
                                                                        << ") does not match cube trade id ("
                                                                        << cube_->ids()[i]);
    }

    if (analytics_["dynamicCredit"]) {
        QL_REQUIRE(cptyCube_, "cptyCube cannot be null when dynamicCredit is ON");
    } else {
        QL_REQUIRE(!cptyCube_, "unexpected non-null cptyCube when dynamicCredit is OFF");
    }

    if (analytics_["dynamicCredit"]) {
        // check portfolio and cptyCube have the same counterparties, in the same order
        QL_REQUIRE(portfolio->counterparties().size() + 1 == cptyCube_->ids().size(),
                   "PostProcess::PostProcess(): portfolio counterparty size ("
                   << portfolio->counterparties().size() << ") does not match cpty cube trade size ("
                   << cptyCube_->ids().size() << ")");
        for (Size i = 0; i < portfolio->counterparties().size(); ++i) {
            QL_REQUIRE(portfolio->counterparties()[i] == cptyCube_->ids()[i],
                       "PostProcess::PostProcess(): portfolio counterparty #"
                       << i << " (id=" << portfolio->counterparties()[i]
                       << ") does not match cube name id ("
                       << cptyCube_->ids()[i]);
        }
        QL_REQUIRE(dvaName == cptyCube_->ids().back(),
                       "PostProcess::PostProcess(): dvaName (" << dvaName
                       << ") does not match cube name id ("
                       << cptyCube_->ids().back());
    }

    Size trades = portfolio->size();
    Size dates = cube_->dates().size(); // (isRegularCubeStorage) ? cube_->dates().size() - 1 : cube_->dates().size();
    Size samples = cube->samples();

    AllocationMethod allocationMethod = parseAllocationMethod(allocMethod);

    /***********************************************
     * Step 0: Netting as of today
     * a) Compute the netting set NPV as of today
     * b) Find the final maturity of the netting set
     */
    LOG("Compute netting set NPVs as of today and netting set maturity");
    map<string, Real> tradeValueToday;
    map<string, Real> nettingSetValueToday, nettingSetPositiveValueToday, nettingSetNegativeValueToday;
    // Don't use Settings::instance().evaluationDate() here, this has moved to simulation end date.
    Date today = market->asofDate();
    LOG("AsOfDate = " << QuantLib::io::iso_date(today));

    vector<Real> times(dates);
    DayCounter dc = ActualActual();

    for (Size i = 0; i < dates; i++)
        times[i] = dc.yearFraction(today, cube_->dates()[i]);

    map<string, string> cidMap, nidMap;
    map<string, Date> matMap;
    for (auto trade : portfolio->trades()) {
        string tradeId = trade->id();
        nidMap[tradeId] = trade->envelope().nettingSetId();
        cidMap[tradeId] = trade->envelope().counterparty();
        if (cidMap[tradeId] != nettingSetManager_->get(nidMap[tradeId])->counterparty()) {
            // changed from QL_REQUIRE in Roland Kapl's original pull request to an ALERT
            ALOG("counterparty from trade (" << cidMap[tradeId]
                                             << "is not the same as counterparty from trade's netting set: "
                                             << nettingSetManager_->get(nidMap[tradeId])->counterparty());
        }
        matMap[tradeId] = trade->maturity();
    }

    for (Size i = 0; i < cube_->ids().size(); ++i) {
        string tradeId = cube_->ids()[i];
        string nettingSetId = nidMap[tradeId];
        string cpId = cidMap[tradeId];
        Real npv = cube_->getT0(i);

        tradeValueToday[tradeId] = npv;
        counterpartyId_[nettingSetId] = cpId;

        if (nettingSetValueToday.find(nettingSetId) == nettingSetValueToday.end()) {
            nettingSetValueToday[nettingSetId] = 0.0;
            nettingSetPositiveValueToday[nettingSetId] = 0.0;
            nettingSetNegativeValueToday[nettingSetId] = 0.0;
        }

        nettingSetValueToday[nettingSetId] += npv;
        if (npv > 0)
            nettingSetPositiveValueToday[nettingSetId] += npv;
        else
            nettingSetNegativeValueToday[nettingSetId] += npv;
    }

    /***************************************************************
     * Step 1: Dynamic Initial Margin calculation
     * Fills DIM cube per netting set that can be
     * - returned to be further analysed
     * - used in collateral calculation
     * - used in MVA calculation
     */
    if (analytics_["dim"] || analytics_["mva"]) {
        QL_REQUIRE(dimCalculator_, "DIM calculator not set");
        dimCalculator_->build();
    }

    /************************************************************
     * Step 2: Trade Exposure and Netting
     * a) Aggregation across scenarios per trade and date
     *    This yields single trade exposure profiles, EPE and ENE
     * b) Aggregation of NPVs within netting sets per date
     *    and scenario. This prepares the netting set exposure
     *    calculation below
     */
    exposureCalculator_ =
        boost::make_shared<ExposureCalculator>(
            portfolio, cube_, cubeInterpretation_,
            market_, analytics_["exerciseNextBreak"], baseCurrency_,
            configuration_, quantile_, calcType_, isRegularCubeStorage,
            analytics_["dynamicCredit"]
        );
    exposureCalculator_->build();

    /******************************************************************
     * Step 3: Netting set exposure and allocation to trades
     *
     * a) Compute all netting set exposure profiles EPE and ENE using
     *    collateral if CSAs are given and active.
     * b) Compute the expected collateral balance for each netting set.
     * c) Allocate each netting set's exposure profile to the trade
     *    level such that the trade exposures add up to the netting
     *    set exposure.
     *    Reference:
     *    Michael Pykhtin & Dan Rosen, Pricing Counterparty Risk
     *    at the Trade Level and CVA Allocations, October 2010
     */
    nettedExposureCalculator_ =
        boost::make_shared<NettedExposureCalculator>(
            portfolio_, market_, cube_, baseCurrency, configuration_, quantile_,
            calcType_, analytics_["dynamicCredit"], nettingSetManager_,
            calcType_ == CollateralExposureHelper::CalculationType::NoLag ?
                         exposureCalculator_->nettingSetCloseOutValue() :
                         exposureCalculator_->nettingSetDefaultValue(),
            scenarioData_, cubeInterpretation_, analytics_["dim"],
            dimCalculator_, fullInitialCollateralisation_
        );
    nettedExposureCalculator_->build();

    /********************************************************
     * Update Stand Alone XVAs
     * needed for some of the simple allocation methods below
     */
    if (analytics_["dynamicCredit"]) {
        cvaCalculator_ = boost::make_shared<DynamicCreditXvaCalculator>(
            portfolio_, market_, configuration_,baseCurrency_, dvaName_,
            fvaBorrowingCurve_, fvaLendingCurve_, analytics_["dim"],
            dimCalculator, exposureCalculator_->exposureCube(),
            nettedExposureCalculator_->exposureCube(), cptyCube_,
            0, 1, 0, 1, 0);
    } else {
        cvaCalculator_ = boost::make_shared<StaticCreditXvaCalculator>(
            portfolio_, market_, configuration_,baseCurrency_, dvaName_,
            fvaBorrowingCurve_, fvaLendingCurve_, analytics_["dim"],
            dimCalculator, exposureCalculator_->exposureCube(),
            nettedExposureCalculator_->exposureCube(),
            0, 1, 0, 1);
    }
    cvaCalculator_->build();

    /***************************
     * Simple allocation methods
     */
    LOG(4);
    if (allocationMethod != AllocationMethod::Marginal) {
        for (auto n : exposureCalculator_->nettingSetDefaultValue()) {
            string nettingSetId = n.first;

            for (Size i = 0; i < trades; ++i) {
                string nid = portfolio->trades()[i]->envelope().nettingSetId();
                if (nid != nettingSetId)
                    continue;
                string tid = portfolio->trades()[i]->id();

                for (Size j = 0; j < dates; ++j) {
                    Date date = cube_->dates()[j];
                    if (allocationMethod == AllocationMethod::RelativeFairValueNet) {
                        // FIXME: What to do when either the pos. or neg. netting set value is zero?
                        QL_REQUIRE(nettingSetPositiveValueToday[nid] > 0.0, "non-zero positive NPV expected");
                        QL_REQUIRE(nettingSetNegativeValueToday[nid] > 0.0, "non-zero negative NPV expected");
                        for (Size k = 0; k < samples; ++k) {
                            Real netEPE = nettedExposureCalculator_->exposureCube()->get(nid, date, k, 0);
                            Real netENE = nettedExposureCalculator_->exposureCube()->get(nid, date, k, 1);
                            exposureCalculator_->exposureCube()->set(
                                netEPE * std::max(tradeValueToday[tid], 0.0) / nettingSetPositiveValueToday[nid],
                                tid, date, k, 2);
                            exposureCalculator_->exposureCube()->set(
                                netENE * -std::max(-tradeValueToday[tid], 0.0) / nettingSetPositiveValueToday[nid],
                                tid, date, k, 3);
                        }
                    } else if (allocationMethod == AllocationMethod::RelativeFairValueGross) {
                        // FIXME: What to do when the netting set value is zero?
                        QL_REQUIRE(nettingSetValueToday[nid] != 0.0, "non-zero netting set value expected");
                        for (Size k = 0; k < samples; ++k) {
                            Real netEPE = nettedExposureCalculator_->exposureCube()->get(nid, date, k, 0);
                            Real netENE = nettedExposureCalculator_->exposureCube()->get(nid, date, k, 1);
                            exposureCalculator_->exposureCube()->set(
                                netEPE * tradeValueToday[tid] / nettingSetPositiveValueToday[nid],
                                tid, date, k, 2);
                            exposureCalculator_->exposureCube()->set(
                                netENE * tradeValueToday[tid] / nettingSetPositiveValueToday[nid],
                                tid, date, k, 3);
                        }
                    } else if (allocationMethod == AllocationMethod::RelativeXVA) {
                        for (Size k = 0; k < samples; ++k) {
                            Real netEPE = nettedExposureCalculator_->exposureCube()->get(nid, date, k, 0);
                            Real netENE = nettedExposureCalculator_->exposureCube()->get(nid, date, k, 1);
                            exposureCalculator_->exposureCube()->set(
                                netEPE * tradeValueToday[tid] / tradeCVA(tid) / cvaCalculator_->nettingSetSumCva(nid),
                                tid, date, k, 2);
                            exposureCalculator_->exposureCube()->set(
                                netENE * tradeValueToday[tid] / tradeCVA(tid) / cvaCalculator_->nettingSetSumDva(nid),
                                tid, date, k, 3);
                        }
                    } else if (allocationMethod == AllocationMethod::None) {
                        DLOG("No allocation from " << nid << " to " << tid << " date " << j);
                        for (Size k = 0; k < samples; ++k) {
                            exposureCalculator_->exposureCube()->set(0.0, tid, date, k, 2);
                            exposureCalculator_->exposureCube()->set(0.0, tid, date, k, 3);
                        }
                    } else
                        QL_FAIL("allocationMethod " << allocationMethod << " not available");
                }
            }
        }
    }

    /********************************************************
     * Update Allocated XVAs
     */
    if (analytics_["dynamicCredit"]) {
        allocatedCvaCalculator_ = boost::make_shared<DynamicCreditXvaCalculator>(
            portfolio_, market_, configuration_,baseCurrency_, dvaName_,
            fvaBorrowingCurve_, fvaLendingCurve_, analytics_["dim"],
            dimCalculator, exposureCalculator_->exposureCube(),
            nettedExposureCalculator_->exposureCube(), cptyCube_,
            0, 1, 0, 1, 0);
    } else {
        cvaCalculator_ = boost::make_shared<StaticCreditXvaCalculator>(
            portfolio_, market_, configuration_,baseCurrency_, dvaName_,
            fvaBorrowingCurve_, fvaLendingCurve_, analytics_["dim"],
            dimCalculator, exposureCalculator_->exposureCube(),
            nettedExposureCalculator_->exposureCube(),
            2, 3, 0, 1);
    }
    allocatedCvaCalculator_->build();

    /********************************************************
     * Cache average EPE and ENE
     */
    for (auto tradeId : tradeIds()) {
        tradeEPE_[tradeId] = exposureCalculator_->epe(tradeId);
        tradeENE_[tradeId] = exposureCalculator_->ene(tradeId);
        allocatedTradeEPE_[tradeId] = exposureCalculator_->allocatedEpe(tradeId);
        allocatedTradeENE_[tradeId] = exposureCalculator_->allocatedEne(tradeId);
    }
    for (auto nettingSetId : nettingSetIds()) {
        netEPE_[nettingSetId] = nettedExposureCalculator_->epe(nettingSetId);
        netENE_[nettingSetId] = nettedExposureCalculator_->ene(nettingSetId);
    }

    /********************************************************
     * Calculate netting set KVA-CCR and KVA-CVA
     */
    updateNettingSetKVA();

}

void PostProcess::updateNettingSetKVA() {
    // Loop over all netting sets
    for (auto nettingSetId : nettingSetIds()) {
        // Init results
        ourNettingSetKVACCR_[nettingSetId] = 0.0;
        theirNettingSetKVACCR_[nettingSetId] = 0.0;
        ourNettingSetKVACVA_[nettingSetId] = 0.0;
        theirNettingSetKVACVA_[nettingSetId] = 0.0;
    }

    if (!analytics_["kva"])
        return;

    vector<Date> dateVector = cube_->dates();
    Size dates = dateVector.size();
    Date today = market_->asofDate();
    Handle<YieldTermStructure> discountCurve = market_->discountCurve(baseCurrency_, configuration_);
    DayCounter dc = ActualActual();

    // Loop over all netting sets
    for (auto nettingSetId : nettingSetIds()) {
        string cid = counterpartyId_[nettingSetId];
        LOG("KVA for netting set " << nettingSetId);

        // Main input are the EPE and ENE profiles, previously computed
        vector<Real> epe = netEPE_[nettingSetId];
        vector<Real> ene = netENE_[nettingSetId];

        // PD from counterparty Dts, floored to avoid 0 ...
        // Today changed to today+1Y to get the one-year PD
        Handle<DefaultProbabilityTermStructure> cvaDts = market_->defaultCurve(cid, configuration_);
        QL_REQUIRE(!cvaDts.empty(), "Default curve missing for counterparty " << cid);
        Real cvaRR = market_->recoveryRate(cid, configuration_)->value();
        Real PD1 = std::max(cvaDts->defaultProbability(today + 1 * Years), 0.000000000001);
        Real LGD1 = (1 - cvaRR);

        Handle<DefaultProbabilityTermStructure> dvaDts;
        Real dvaRR = 0.0;
        Real PD2 = 0;
        if (dvaName_ != "") {
            dvaDts = market_->defaultCurve(dvaName_, configuration_);
            dvaRR = market_->recoveryRate(dvaName_, configuration_)->value();
            PD2 = std::max(dvaDts->defaultProbability(today + 1 * Years), 0.000000000001);
        } else {
            ALOG("dvaName not specified, own PD set to zero for their KVA calculation");
        }
        Real LGD2 = (1 - dvaRR);

        // Granularity adjustment, Gordy (2004):
        Real rho1 = 0.12 * (1 - std::exp(-50 * PD1)) / (1 - std::exp(-50)) +
                    0.24 * (1 - (1 - std::exp(-50 * PD1)) / (1 - std::exp(-50)));
        Real rho2 = 0.12 * (1 - std::exp(-50 * PD2)) / (1 - std::exp(-50)) +
                    0.24 * (1 - (1 - std::exp(-50 * PD2)) / (1 - std::exp(-50)));

        // Basel II internal rating based (IRB) estimate of worst case PD:
        // Large homogeneous pool (LHP) approximation of Vasicek (1997)
        InverseCumulativeNormal icn;
        CumulativeNormalDistribution cnd;
        Real PD99_1 = cnd((icn(PD1) + std::sqrt(rho1) * icn(0.999)) / (std::sqrt(1 - rho1))) - PD1;
        Real PD99_2 = cnd((icn(PD2) + std::sqrt(rho2) * icn(0.999)) / (std::sqrt(1 - rho2))) - PD2;

        // KVA regulatory PD, worst case PD, floored at 0.03 for corporates and banks, not floored for sovereigns
        Real kva99PD1 = std::max(PD99_1, kvaTheirPdFloor_);
        Real kva99PD2 = std::max(PD99_2, kvaOurPdFloor_);

        // Factor B(PD) for the maturity adjustment factor, B(PD) = (0.11852 - 0.05478 * ln(PD)) ^ 2
        Real kvaMatAdjB1 = std::pow((0.11852 - 0.05478 * std::log(PD1)), 2.0);
        Real kvaMatAdjB2 = std::pow((0.11852 - 0.05478 * std::log(PD2)), 2.0);

        DLOG("Our KVA-CCR " << nettingSetId << ": PD=" << PD1);
        DLOG("Our KVA-CCR " << nettingSetId << ": LGD=" << LGD1);
        DLOG("Our KVA-CCR " << nettingSetId << ": rho=" << rho1);
        DLOG("Our KVA-CCR " << nettingSetId << ": PD99=" << PD99_1);
        DLOG("Our KVA-CCR " << nettingSetId << ": PD Floor=" << kvaTheirPdFloor_);
        DLOG("Our KVA-CCR " << nettingSetId << ": Floored PD99=" << kva99PD1);
        DLOG("Our KVA-CCR " << nettingSetId << ": B(PD)=" << kvaMatAdjB1);

        DLOG("Their KVA-CCR " << nettingSetId << ": PD=" << PD2);
        DLOG("Their KVA-CCR " << nettingSetId << ": LGD=" << LGD2);
        DLOG("Their KVA-CCR " << nettingSetId << ": rho=" << rho2);
        DLOG("Their KVA-CCR " << nettingSetId << ": PD99=" << PD99_2);
        DLOG("Their KVA-CCR " << nettingSetId << ": PD Floor=" << kvaOurPdFloor_);
        DLOG("Their KVA-CCR " << nettingSetId << ": Floored PD99=" << kva99PD2);
        DLOG("Their KVA-CCR " << nettingSetId << ": B(PD)=" << kvaMatAdjB2);

        for (Size j = 0; j < dates; ++j) {
            Date d0 = j == 0 ? today : cube_->dates()[j - 1];
            Date d1 = cube_->dates()[j];

            // Preprocess:
            // 1) Effective maturity from effective expected exposure as of time j
            //    Index _1 corresponds to our perspective, index _2 to their perspective.
            // 2) Basel EEPE as of time j, i.e. as time averge over EEE, starting at time j
            // More accuracy may be achieved here by using a Longstaff-Schwartz method / regression
            Real eee_kva_1 = 0.0, eee_kva_2 = 0.0;
            Real effMatNumer1 = 0.0, effMatNumer2 = 0.0;
            Real effMatDenom1 = 0.0, effMatDenom2 = 0.0;
            Real eepe_kva_1 = 0, eepe_kva_2 = 0.0;
            Size kmax = j, count = 0;
            // Cut off index for EEPE/EENE calculation: One year ahead
            while (dateVector[kmax] < dateVector[j] + 1 * Years + 4 * Days && kmax < dates - 1)
                kmax++;
            Real sumdt = 0.0, eee1_b = 0.0, eee2_b = 0.0;
            for (Size k = j; k < dates; ++k) {
                Date d2 = cube_->dates()[k];
                Date prevDate = k == 0 ? today : dateVector[k - 1];

                eee_kva_1 = std::max(eee_kva_1, epe[k + 1]);
                eee_kva_2 = std::max(eee_kva_2, ene[k + 1]);

                // Components of the KVA maturity adjustment MA as of time j
                if (dc.yearFraction(d1, d2) > 1.0) {
                    effMatNumer1 += epe[k + 1] * dc.yearFraction(prevDate, d2);
                    effMatNumer2 += ene[k + 1] * dc.yearFraction(prevDate, d2);
                }
                if (dc.yearFraction(d1, d2) <= 1.0) {
                    effMatDenom1 += eee_kva_1 * dc.yearFraction(prevDate, d2);
                    effMatDenom2 += eee_kva_2 * dc.yearFraction(prevDate, d2);
                }

                if (k < kmax) {
                    Real dt = dc.yearFraction(cube_->dates()[k], cube_->dates()[k + 1]);
                    sumdt += dt;
                    Real epe_b = epe[k + 1] / discountCurve->discount(dateVector[k]);
                    Real ene_b = ene[k + 1] / discountCurve->discount(dateVector[k]);
                    eee1_b = std::max(epe_b, eee1_b);
                    eee2_b = std::max(ene_b, eee2_b);
                    eepe_kva_1 += eee1_b * dt;
                    eepe_kva_2 += eee2_b * dt;
                    count++;
                }
            }

            // Normalize EEPE/EENE calculation
            eepe_kva_1 = count > 0 ? eepe_kva_1 / sumdt : 0.0;
            eepe_kva_2 = count > 0 ? eepe_kva_2 / sumdt : 0.0;

            // KVA CCR using the IRB risk weighted asset method and IMM:
            // KVA effective maturity of the nettingSet, capped at 5
            Real kvaNWMaturity1 = std::min(1.0 + (effMatDenom1 == 0.0 ? 0.0 : effMatNumer1 / effMatDenom1), 5.0);
            Real kvaNWMaturity2 = std::min(1.0 + (effMatDenom2 == 0.0 ? 0.0 : effMatNumer2 / effMatDenom2), 5.0);

            // Maturity adjustment factor for the RWA method:
            // MA(PD, M) = (1 + (M - 2.5) * B(PD)) / (1 - 1.5 * B(PD)), capped at 5, floored at 1, M = effective
            // maturity
            Real kvaMatAdj1 =
                std::max(std::min((1.0 + (kvaNWMaturity1 - 2.5) * kvaMatAdjB1) / (1.0 - 1.5 * kvaMatAdjB1), 5.0), 1.0);
            Real kvaMatAdj2 =
                std::max(std::min((1.0 + (kvaNWMaturity2 - 2.5) * kvaMatAdjB2) / (1.0 - 1.5 * kvaMatAdjB2), 5.0), 1.0);

            // CCR Capital: RC = EAD x LGD x PD99.9 x MA(PD, M); EAD = alpha x EEPE(t) (approximated by EPE here);
            Real kvaRC1 = kvaAlpha_ * eepe_kva_1 * LGD1 * kva99PD1 * kvaMatAdj1;
            Real kvaRC2 = kvaAlpha_ * eepe_kva_2 * LGD2 * kva99PD2 * kvaMatAdj2;

            // Expected risk capital discounted at capital discount rate
            Real kvaCapitalDiscount = 1 / std::pow(1 + kvaCapitalDiscountRate_, dc.yearFraction(today, d0));
            Real kvaCCRIncrement1 =
                kvaRC1 * kvaCapitalDiscount * dc.yearFraction(d0, d1) * kvaCapitalHurdle_ * kvaRegAdjustment_;
            Real kvaCCRIncrement2 =
                kvaRC2 * kvaCapitalDiscount * dc.yearFraction(d0, d1) * kvaCapitalHurdle_ * kvaRegAdjustment_;

            ourNettingSetKVACCR_[nettingSetId] += kvaCCRIncrement1;
            theirNettingSetKVACCR_[nettingSetId] += kvaCCRIncrement2;

            DLOG("Our KVA-CCR for " << nettingSetId << ": " << j << " EEPE=" << setprecision(2) << eepe_kva_1
                                    << " EPE=" << epe[j] << " RC=" << kvaRC1 << " M=" << setprecision(6)
                                    << kvaNWMaturity1 << " MA=" << kvaMatAdj1 << " Cost=" << setprecision(2)
                                    << kvaCCRIncrement1 << " KVA=" << ourNettingSetKVACCR_[nettingSetId]);
            DLOG("Their KVA-CCR for " << nettingSetId << ": " << j << " EENE=" << eepe_kva_2 << " ENE=" << ene[j]
                                      << " RC=" << kvaRC2 << " M=" << setprecision(6) << kvaNWMaturity2
                                      << " MA=" << kvaMatAdj2 << " Cost=" << setprecision(2) << kvaCCRIncrement2
                                      << " KVA=" << theirNettingSetKVACCR_[nettingSetId]);

            // CVA Capital
            // effective maturity without cap at 5, DF set to 1 for IMM banks
            // TODO: Set MA in CCR capital calculation to 1
            Real kvaCvaMaturity1 = 1.0 + (effMatDenom1 == 0.0 ? 0.0 : effMatNumer1 / effMatDenom1);
            Real kvaCvaMaturity2 = 1.0 + (effMatDenom2 == 0.0 ? 0.0 : effMatNumer2 / effMatDenom2);
            Real scva1 = kvaTheirCvaRiskWeight_ * kvaCvaMaturity1 * eepe_kva_1;
            Real scva2 = kvaOurCvaRiskWeight_ * kvaCvaMaturity2 * eepe_kva_2;
            Real kvaCVAIncrement1 =
                scva1 * kvaCapitalDiscount * dc.yearFraction(d0, d1) * kvaCapitalHurdle_ * kvaRegAdjustment_;
            Real kvaCVAIncrement2 =
                scva2 * kvaCapitalDiscount * dc.yearFraction(d0, d1) * kvaCapitalHurdle_ * kvaRegAdjustment_;

            DLOG("Our KVA-CVA for " << nettingSetId << ": " << j << " EEPE=" << eepe_kva_1 << " SCVA=" << scva1
                                    << " Cost=" << kvaCVAIncrement1);
            DLOG("Their KVA-CVA for " << nettingSetId << ": " << j << " EENE=" << eepe_kva_2 << " SCVA=" << scva2
                                      << " Cost=" << kvaCVAIncrement2);

            ourNettingSetKVACVA_[nettingSetId] += kvaCVAIncrement1;
            theirNettingSetKVACVA_[nettingSetId] += kvaCVAIncrement2;
        }
    }
}

const vector<Real>& PostProcess::tradeEPE(const string& tradeId) {
    QL_REQUIRE(tradeEPE_.find(tradeId) != tradeEPE_.end(), "Trade " << tradeId << " not found in exposure map");
    return tradeEPE_[tradeId];
}

const vector<Real>& PostProcess::tradeENE(const string& tradeId) {
    QL_REQUIRE(tradeENE_.find(tradeId) != tradeENE_.end(), "Trade " << tradeId << " not found in exposure map");
    return tradeENE_[tradeId];
}

const vector<Real>& PostProcess::tradeEE_B(const string& tradeId) {
    return exposureCalculator_->ee_b(tradeId);
}

const Real& PostProcess::tradeEPE_B(const string& tradeId) {
    return exposureCalculator_->epe_b(tradeId);
}

const vector<Real>& PostProcess::tradeEEE_B(const string& tradeId) {
    return exposureCalculator_->eee_b(tradeId);
}

const Real& PostProcess::tradeEEPE_B(const string& tradeId) {
    return exposureCalculator_->eepe_b(tradeId);
}

const vector<Real>& PostProcess::tradePFE(const string& tradeId) {
    return exposureCalculator_->pfe(tradeId);
}

const vector<Real>& PostProcess::netEPE(const string& nettingSetId) {
    QL_REQUIRE(netEPE_.find(nettingSetId) != netEPE_.end(),
               "Netting set " << nettingSetId << " not found in exposure map");
    return netEPE_[nettingSetId];
}

const vector<Real>& PostProcess::netENE(const string& nettingSetId) {
    QL_REQUIRE(netENE_.find(nettingSetId) != netENE_.end(),
               "Netting set " << nettingSetId << " not found in exposure map");
    return netENE_[nettingSetId];
}

const vector<Real>& PostProcess::netEE_B(const string& nettingSetId) {
    return nettedExposureCalculator_->ee_b(nettingSetId);
}

const Real& PostProcess::netEPE_B(const string& nettingSetId) {
    return nettedExposureCalculator_->epe_b(nettingSetId);
}

const vector<Real>& PostProcess::netEEE_B(const string& nettingSetId) {
    return nettedExposureCalculator_->eee_b(nettingSetId);
}

const Real& PostProcess::netEEPE_B(const string& nettingSetId) {
    return nettedExposureCalculator_->eepe_b(nettingSetId);
}

const vector<Real>& PostProcess::netPFE(const string& nettingSetId) {
    return nettedExposureCalculator_->pfe(nettingSetId);
}

const vector<Real>& PostProcess::expectedCollateral(const string& nettingSetId) {
    return nettedExposureCalculator_->expectedCollateral(nettingSetId);
}

const vector<Real>& PostProcess::colvaIncrements(const string& nettingSetId) {
    return nettedExposureCalculator_->colvaIncrements(nettingSetId);
}

const vector<Real>& PostProcess::collateralFloorIncrements(const string& nettingSetId) {
    return nettedExposureCalculator_->colvaIncrements(nettingSetId);
}

const vector<Real>& PostProcess::allocatedTradeEPE(const string& tradeId) {
    QL_REQUIRE(allocatedTradeEPE_.find(tradeId) != allocatedTradeEPE_.end(),
               "Trade " << tradeId << " not found in exposure map");
    return allocatedTradeEPE_[tradeId];
}

const vector<Real>& PostProcess::allocatedTradeENE(const string& tradeId) {
    QL_REQUIRE(allocatedTradeENE_.find(tradeId) != allocatedTradeENE_.end(),
               "Trade " << tradeId << " not found in exposure map");
    return allocatedTradeENE_[tradeId];
}

Real PostProcess::tradeCVA(const string& tradeId) {
    return cvaCalculator_->tradeCva(tradeId);
}

Real PostProcess::tradeDVA(const string& tradeId) {
    return cvaCalculator_->tradeDva(tradeId);
}

Real PostProcess::tradeMVA(const string& tradeId) {
    return cvaCalculator_->tradeMva(tradeId);
}

Real PostProcess::tradeFBA(const string& tradeId) {
    return cvaCalculator_->tradeFba(tradeId);
}

Real PostProcess::tradeFCA(const string& tradeId) {
    return cvaCalculator_->tradeFca(tradeId);
}

Real PostProcess::tradeFBA_exOwnSP(const string& tradeId) {
    return cvaCalculator_->tradeFba_exOwnSp(tradeId);
}

Real PostProcess::tradeFCA_exOwnSP(const string& tradeId) {
    return cvaCalculator_->tradeFca_exOwnSp(tradeId);
}

Real PostProcess::tradeFBA_exAllSP(const string& tradeId) {
    return cvaCalculator_->tradeFba_exAllSp(tradeId);
}

Real PostProcess::tradeFCA_exAllSP(const string& tradeId) {
    return cvaCalculator_->tradeFca_exAllSp(tradeId);
}

Real PostProcess::nettingSetCVA(const string& nettingSetId) {
    return cvaCalculator_->nettingSetCva(nettingSetId);
}

Real PostProcess::nettingSetDVA(const string& nettingSetId) {
    return cvaCalculator_->nettingSetDva(nettingSetId);
}

Real PostProcess::nettingSetMVA(const string& nettingSetId) {
    return cvaCalculator_->nettingSetMva(nettingSetId);

}

Real PostProcess::nettingSetFBA(const string& nettingSetId) {
    return cvaCalculator_->nettingSetFba(nettingSetId);

}

Real PostProcess::nettingSetFCA(const string& nettingSetId) {
    return cvaCalculator_->nettingSetFca(nettingSetId);

}

Real PostProcess::nettingSetOurKVACCR(const string& nettingSetId) {
    QL_REQUIRE(ourNettingSetKVACCR_.find(nettingSetId) != ourNettingSetKVACCR_.end(),
               "NettingSetId " << nettingSetId << " not found in nettingSet KVACCR map");
    return ourNettingSetKVACCR_[nettingSetId];
}

Real PostProcess::nettingSetTheirKVACCR(const string& nettingSetId) {
    QL_REQUIRE(theirNettingSetKVACCR_.find(nettingSetId) != theirNettingSetKVACCR_.end(),
               "NettingSetId " << nettingSetId << " not found in nettingSet KVACCR map");
    return theirNettingSetKVACCR_[nettingSetId];
}

Real PostProcess::nettingSetOurKVACVA(const string& nettingSetId) {
    QL_REQUIRE(ourNettingSetKVACVA_.find(nettingSetId) != ourNettingSetKVACVA_.end(),
               "NettingSetId " << nettingSetId << " not found in nettingSet KVACVA map");
    return ourNettingSetKVACVA_[nettingSetId];
}

Real PostProcess::nettingSetTheirKVACVA(const string& nettingSetId) {
    QL_REQUIRE(theirNettingSetKVACVA_.find(nettingSetId) != theirNettingSetKVACVA_.end(),
               "NettingSetId " << nettingSetId << " not found in nettingSet KVACVA map");
    return theirNettingSetKVACVA_[nettingSetId];
}

Real PostProcess::nettingSetFBA_exOwnSP(const string& nettingSetId) {
    return cvaCalculator_->nettingSetFba_exOwnSp(nettingSetId);
}

Real PostProcess::nettingSetFCA_exOwnSP(const string& nettingSetId) {
    return cvaCalculator_->nettingSetFca_exOwnSp(nettingSetId);
}

Real PostProcess::nettingSetFBA_exAllSP(const string& nettingSetId) {
    return cvaCalculator_->nettingSetFba_exAllSp(nettingSetId);
}

Real PostProcess::nettingSetFCA_exAllSP(const string& nettingSetId) {
    return cvaCalculator_->nettingSetFca_exAllSp(nettingSetId);
}

Real PostProcess::allocatedTradeCVA(const string& allocatedTradeId) {
    return allocatedCvaCalculator_->tradeCva(allocatedTradeId);
}

Real PostProcess::allocatedTradeDVA(const string& allocatedTradeId) {
    return allocatedCvaCalculator_->tradeDva(allocatedTradeId);
}

Real PostProcess::nettingSetCOLVA(const string& nettingSetId) {
    return nettedExposureCalculator_->colva(nettingSetId);
}

Real PostProcess::nettingSetCollateralFloor(const string& nettingSetId) {
    return nettedExposureCalculator_->collateralFloor(nettingSetId);
}

void PostProcess::exportDimEvolution(ore::data::Report& dimEvolutionReport) {
    dimCalculator_->exportDimEvolution(dimEvolutionReport);
}
  
void PostProcess::exportDimRegression(const std::string& nettingSet, const std::vector<Size>& timeSteps,
                                      const std::vector<boost::shared_ptr<ore::data::Report>>& dimRegReports) {

    boost::shared_ptr<RegressionDynamicInitialMarginCalculator> regCalc =
        boost::dynamic_pointer_cast<RegressionDynamicInitialMarginCalculator>(dimCalculator_);

    if (regCalc)
        regCalc->exportDimRegression(nettingSet, timeSteps, dimRegReports);
}

} // namespace analytics
} // namespace ore
