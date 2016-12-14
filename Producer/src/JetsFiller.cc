#include "../interface/JetsFiller.h"

#include "FWCore/Framework/interface/ESHandle.h"
#include "FWCore/ServiceRegistry/interface/Service.h"
#include "FWCore/Utilities/interface/RandomNumberGenerator.h"

#include "DataFormats/PatCandidates/interface/Jet.h"
#include "DataFormats/JetReco/interface/GenJet.h"
#include "DataFormats/Math/interface/deltaR.h"

#include "CLHEP/Random/RandomEngine.h"
#include "CLHEP/Random/RandGauss.h"

#include "CondFormats/JetMETObjects/interface/JetCorrectorParameters.h"
#include "CondFormats/JetMETObjects/interface/JetCorrectionUncertainty.h"
#include "JetMETCorrections/Objects/interface/JetCorrectionsRecord.h"
#include "JetMETCorrections/Objects/interface/JetCorrector.h"
#include "JetMETCorrections/Modules/interface/JetResolution.h"

JetsFiller::JetsFiller(std::string const& _name, edm::ParameterSet const& _cfg, edm::ConsumesCollector& _coll) :
  FillerBase(_name, _cfg),
  csvTag_(getParameter_<std::string>(_cfg, "csv", "")),
  puidTag_(getParameter_<std::string>(_cfg, "puid", "")),
  R_(getParameter_<double>(_cfg, "R", 0.4)),
  minPt_(getParameter_<double>(_cfg, "minPt", 15.)),
  maxEta_(getParameter_<double>(_cfg, "maxEta", 4.7)),
  fillConstituents_(getParameter_<bool>(_cfg, "fillConstituents", false))
{
  if (_name == "chsAK4Jets")
    outputType_ = kCHSAK4;
  else if (_name == "puppiAK4Jets")
    outputType_ = kPuppiAK4;
  if (_name == "chsAK8Jets")
    outputType_ = kCHSAK8;
  if (_name == "puppiAK8Jets")
    outputType_ = kPuppiAK8;
  if (_name == "chsCA15Jets")
    outputType_ = kCHSCA15;
  if (_name == "puppiCA15Jets")
    outputType_ = kPuppiCA15;

  getToken_(jetsToken_, _cfg, _coll, "jets");
  getToken_(qglToken_, _cfg, _coll, "qgl", false);
  if (!isRealData_) {
    getToken_(genJetsToken_, _cfg, _coll, "genJets", false);
    getToken_(rhoToken_, _cfg, _coll, "rho", "rho");
  }
}

JetsFiller::~JetsFiller()
{
  delete jecUncertainty_;
}

void
JetsFiller::branchNames(panda::utils::BranchList& _eventBranches, panda::utils::BranchList&) const
{
  if (isRealData_)
    _eventBranches.emplace_back("!" + getName() + ".matchedGenJet_");

  if (isRealData_ || outputType_ == kCHSCA15 || outputType_ == kPuppiCA15) {
    char const* genBranches[] = {
      ".ptSmear",
      ".ptSmearUp",
      ".ptSmearDown"
    };
    for (char const* b : genBranches)
      _eventBranches.emplace_back("!" + getName() + b);
  }

  if (outputType_ == kCHSCA15 || outputType_ == kPuppiCA15) {
    char const* genBranches[] = {
      ".ptCorrUp",
      ".ptCorrDown",
      ".rawPt"
    };
    for (char const* b : genBranches)
      _eventBranches.emplace_back("!" + getName() + b);
  }

  if (puidTag_.empty())
    _eventBranches.emplace_back("!" + getName() + ".puid");

  if (csvTag_.empty())
    _eventBranches.emplace_back("!" + getName() + ".csv");

  if (qglToken_.second.isUninitialized())
    _eventBranches.emplace_back("!" + getName() + ".qgl");

  if (!fillConstituents_)
    _eventBranches.emplace_back("!" + getName() + ".constituents_");
}

void
JetsFiller::fill(panda::Event& _outEvent, edm::Event const& _inEvent, edm::EventSetup const& _setup)
{
  auto& inJets(getProduct_(_inEvent, jetsToken_));

  panda::JetCollection* pOutJets(0);
  std::string jetCorrName;
  switch (outputType_) {
  case kCHSAK4:
    pOutJets = &_outEvent.chsAK4Jets;
    jetCorrName = "AK4PFchs";
    break;
  case kPuppiAK4:
    pOutJets = &_outEvent.puppiAK4Jets;
    jetCorrName = "AK4PFPuppi";
    break;
  case kCHSAK8:
    pOutJets = &_outEvent.chsAK8Jets;
    jetCorrName = "AK8PFchs";
    break;
  case kPuppiAK8:
    pOutJets = &_outEvent.puppiAK8Jets;
    jetCorrName = "AK8PFPuppi";
    break;
  case kCHSCA15:
    pOutJets = &_outEvent.chsCA15Jets;
    break;
  case kPuppiCA15:
    pOutJets = &_outEvent.puppiCA15Jets;
    break;
  }
  panda::JetCollection& outJets(*pOutJets);

  if (!jecUncertainty_ && !jetCorrName.empty()) {
    edm::ESHandle<JetCorrectorParametersCollection> jecColl;
    _setup.get<JetCorrectionsRecord>().get(jetCorrName, jecColl);
    jecUncertainty_ = new JetCorrectionUncertainty((*jecColl)["Uncertainty"]);
  }

  JME::JetResolution ptRes;
  JME::JetResolutionScaleFactor ptResSF;
  reco::GenJetCollection const* genJets(0);
  double rho(0.);
  CLHEP::RandGauss* random(0);
  
  if (!isRealData_ && !jetCorrName.empty()) {
    ptRes = JME::JetResolution::get(_setup, jetCorrName + "_pt");
    ptResSF = JME::JetResolutionScaleFactor::get(_setup, jetCorrName);
    if (!genJetsToken_.second.isUninitialized())
      genJets = &getProduct_(_inEvent, genJetsToken_);
    rho = getProduct_(_inEvent, rhoToken_);
    random = new CLHEP::RandGauss(edm::Service<edm::RandomNumberGenerator>()->getEngine(_inEvent.streamID()));
  }

  FloatMap const* inQGL(0);
  if (!qglToken_.second.isUninitialized())
    inQGL = &getProduct_(_inEvent, qglToken_);

  std::vector<edm::Ptr<reco::Jet>> ptrList;

  unsigned iJet(-1);
  for (auto& inJet : inJets) {
    ++iJet;

    if (inJet.pt() < minPt_)
      continue;

    double absEta(std::abs(inJet.eta()));
    if (absEta > 4.7)
      continue;

    auto inRef(inJets.refAt(iJet));

    // Forking for MINIAOD jets (not that we have implementation for reco::PFJet though)
    if (dynamic_cast<pat::Jet const*>(&inJet)) {
      auto& patJet(static_cast<pat::Jet const&>(inJet));

      double nhf(patJet.neutralHadronEnergyFraction());
      double nef(patJet.neutralEmEnergyFraction());
      double chf(patJet.chargedHadronEnergyFraction());
      double cef(patJet.chargedEmEnergyFraction());
      unsigned nc(patJet.chargedMultiplicity());
      unsigned nn(patJet.neutralMultiplicity());
      unsigned nd(patJet.numberOfDaughters());
      bool loose(false);
      bool tight(false);
      bool monojet(false);

      if (absEta <= 2.7) {
        loose = nhf < 0.99 && nef < 0.99 && nd > 1;
        tight = nhf < 0.9 && nef < 0.9 && nd > 1;
        monojet = nhf < 0.8 && nef < 0.99 && nd > 1;

        if (absEta <= 2.4) {
          loose = loose && chf > 0. && nc > 0 && cef < 0.99;
          tight = tight && chf > 0. && nc > 0 && cef < 0.99;
          monojet = monojet && chf > 0.1 && nc > 0 && cef < 0.99;
        }
      }
      else if (absEta <= 3.)
        loose = tight = nef < 0.9 && nn > 2;
      else
        loose = tight = nef < 0.9 && nn > 2;

      auto& outJet(outJets.create_back());

      fillP4(outJet, inJet);

      if (outputType_ != kCHSCA15 && outputType_ != kPuppiCA15)
        outJet.rawPt = patJet.pt() * patJet.jecFactor("Uncorrected");

      if (jecUncertainty_) {
        jecUncertainty_->setJetEta(inJet.eta());
        jecUncertainty_->setJetPt(inJet.pt());
        outJet.ptCorrUp = outJet.pt * (1. + jecUncertainty_->getUncertainty(true));
        jecUncertainty_->setJetEta(inJet.eta());
        jecUncertainty_->setJetPt(inJet.pt());
        outJet.ptCorrDown = outJet.pt * (1. - jecUncertainty_->getUncertainty(false));
      }

      if (!isRealData_ && !jetCorrName.empty()) {
        JME::JetParameters resParams({{JME::Binning::JetPt, inJet.pt()}, {JME::Binning::JetEta, inJet.eta()}, {JME::Binning::Rho, rho}});
        double res(ptRes.getResolution(resParams) * inJet.pt());

        JME::JetParameters sfParams({{JME::Binning::JetEta, inJet.eta()}});
        double sf(ptResSF.getScaleFactor(sfParams));
        double sfUp(ptResSF.getScaleFactor(sfParams, Variation::UP));
        double sfDown(ptResSF.getScaleFactor(sfParams, Variation::DOWN));

        bool matched(false);
        if (genJets) {
          for (auto& genJet : *genJets) {
            double dpt(inJet.pt() - genJet.pt());
            if (reco::deltaR(genJet, inJet) < R_ * 0.5 && std::abs(dpt) < res * 3.) {
              matched = true;
              outJet.ptSmear = std::max(0., genJet.pt() + sf * dpt);
              outJet.ptSmearUp = std::max(0., genJet.pt() + sfUp * dpt);
              outJet.ptSmearDown = std::max(0., genJet.pt() + sfDown * dpt);
              break;
            }
          }
        }

        if (!matched) {
          double resShift(std::sqrt(sf * sf - 1.));
          outJet.ptSmear = (*random)(inJet.pt(), resShift * res);
          // Smear the jet in the same direction, just with different SF
          outJet.ptSmearUp = inJet.pt() + (outJet.ptSmear - inJet.pt()) * std::sqrt(sfUp * sfUp - 1.) / resShift;
          outJet.ptSmearDown = inJet.pt() + (outJet.ptSmear - inJet.pt()) * std::sqrt(sfDown * sfDown - 1.) / resShift;
        }
      }

      if (!csvTag_.empty())
        outJet.csv = patJet.bDiscriminator(csvTag_);
      if (inQGL)
        outJet.qgl = (*inQGL)[inRef];
      outJet.nhf = nhf;
      outJet.chf = chf;
      if (!puidTag_.empty())
        outJet.puid = patJet.userFloat(puidTag_);
      outJet.loose = loose;
      outJet.tight = tight;
      outJet.monojet = monojet;
    }

    ptrList.push_back(inJets.ptrAt(iJet));
  }

  // sort the output jets
  auto originalIndices(outJets.sort(panda::ptGreater));

  // export panda <-> reco mapping

  auto& objectMap(objectMap_->get<reco::Jet, panda::Jet>());

  for (unsigned iP(0); iP != outJets.size(); ++iP) {
    auto& outJet(outJets[iP]);
    unsigned idx(originalIndices[iP]);
    objectMap.add(ptrList[idx], outJet);
  }

  fillDetails_(_outEvent, _inEvent, _setup);

  delete random;
}

void
JetsFiller::setRefs(ObjectMapStore const& _objectMaps)
{
  if (fillConstituents_) {
    auto& jetMap(objectMap_->get<reco::Jet, panda::Jet>());

    auto& pfMap(_objectMaps.at("pfCandidates").get<reco::Candidate, panda::PFCand>().fwdMap);

    for (auto& link : jetMap.fwdMap) { // edm -> panda
      auto& inJet(*link.first);
      auto& outJet(*link.second);

      for (auto&& ptr : inJet.getJetConstituents()) {
        reco::CandidatePtr p(ptr);
        while (p->sourceCandidatePtr(0).isNonnull())
          p = p->sourceCandidatePtr(0);
        outJet.constituents.push_back(*pfMap.at(p));
      }
    }
  }
}

DEFINE_TREEFILLER(JetsFiller);
