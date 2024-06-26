// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

///
/// \file    ReferenceComparatorTask.xx
/// \author  Andrea Ferrero
/// \brief   Post-processing task that compares a given set of plots with reference ones
///

#include "Common/ReferenceComparatorTask.h"
#include "Common/ReferenceComparatorPlot.h"
#include "Common/TH1Ratio.h"
#include "Common/TH2Ratio.h"
#include "QualityControl/QcInfoLogger.h"
#include "QualityControl/MonitorObject.h"
#include "QualityControl/DatabaseInterface.h"
#include "QualityControl/ActivityHelpers.h"
// ROOT
#include <TClass.h>
#include <TH1.h>
#include <TH2.h>
#include <TCanvas.h>

using namespace o2::quality_control::postprocessing;
using namespace o2::quality_control::core;
using namespace o2::quality_control;

namespace o2::quality_control_modules::common
{

static bool splitObjectPath(const std::string& fullPath, std::string& path, std::string& name)
{
  std::string delimiter = "/";
  std::string det;
  size_t pos = fullPath.rfind(delimiter);
  if (pos == std::string::npos) {
    return false;
  }
  path = fullPath.substr(0, pos);
  name = fullPath.substr(pos + 1);
  return true;
}

//_________________________________________________________________________________________
//
// Helper function for retrieving the last MonitorObject for a give run number

static std::shared_ptr<MonitorObject> getMOFromRun(repository::DatabaseInterface* qcdb, const std::string& fullPath, uint32_t run, Activity activity)
{
  uint64_t timeStamp = 0;
  activity.mId = run;
  const auto filterMetadata = activity_helpers::asDatabaseMetadata(activity, false);
  const auto objectValidity = qcdb->getLatestObjectValidity(activity.mProvenance + "/" + fullPath, filterMetadata);
  if (objectValidity.isValid()) {
    timeStamp = objectValidity.getMax() - 1;
  } else {
    ILOG(Warning, Devel) << "Could not find the object '" << fullPath << "' for run " << activity.mId << ENDM;
    return nullptr;
  }

  std::string path;
  std::string name;
  if (!splitObjectPath(fullPath, path, name)) {
    return nullptr;
  }
  // retrieve MO from CCDB
  auto mo = qcdb->retrieveMO(path, name, timeStamp, activity);

  return mo;
}

//_________________________________________________________________________________________
// Helper function for retrieving a MonitorObject from the QCDB, in the form of a std::pair<std::shared_ptr<MonitorObject>, bool>
// A non-null MO is returned in the first element of the pair if the MO is found in the QCDB
// The second element of the pair is set to true if the MO has a time stamp more recent than a user-supplied threshold

static std::pair<std::shared_ptr<MonitorObject>, bool> getMO(repository::DatabaseInterface& qcdb, const std::string& fullPath, Trigger trigger, long notOlderThan)
{
  // find the time-stamp of the most recent object matching the current activity
  // if ignoreActivity is true the activity matching criteria are not applied
  auto objectTimestamp = trigger.timestamp;
  const auto filterMetadata = activity_helpers::asDatabaseMetadata(trigger.activity, false);
  const auto objectValidity = qcdb.getLatestObjectValidity(trigger.activity.mProvenance + "/" + fullPath, filterMetadata);
  if (objectValidity.isValid()) {
    objectTimestamp = objectValidity.getMax() - 1;
  } else {
    ILOG(Warning, Devel) << "Could not find the object '" << fullPath << "' for activity " << trigger.activity << ENDM;
    return { nullptr, false };
  }

  std::string path;
  std::string name;
  if (!splitObjectPath(fullPath, path, name)) {
    return { nullptr, false };
  }
  // retrieve QO from CCDB - do not associate to trigger activity if ignoreActivity is true
  auto qo = qcdb.retrieveMO(path, name, objectTimestamp, trigger.activity);
  if (!qo) {
    return { nullptr, false };
  }

  long elapsed = static_cast<long>(trigger.timestamp) - objectTimestamp;
  // check if the object is not older than a given number of milliseconds
  if (elapsed > notOlderThan) {
    ILOG(Warning, Devel) << "Object '" << fullPath << "' for activity " << trigger.activity << " is too old: " << elapsed << " > " << notOlderThan << ENDM;
    return { qo, false };
  }

  return { qo, true };
}

//_________________________________________________________________________________________
//
// Get the reference plot for a given MonitorObject path

std::shared_ptr<MonitorObject> ReferenceComparatorTask::getReferencePlot(repository::DatabaseInterface& qcdb, std::string fullPath, Activity activity)
{
  return getMOFromRun(&qcdb, fullPath, mReferenceRun, activity);
}

//_________________________________________________________________________________________

void ReferenceComparatorTask::configure(const boost::property_tree::ptree& config)
{
  mConfig = ReferenceComparatorTaskConfig(getID(), config);
}

//_________________________________________________________________________________________

void ReferenceComparatorTask::initialize(quality_control::postprocessing::Trigger t, framework::ServiceRegistryRef services)
{
  // reset all existing objects
  mPlotNames.clear();
  mReferencePlots.clear();
  mHistograms.clear();

  auto& qcdb = services.get<repository::DatabaseInterface>();
  mNotOlderThan = std::stoi(mCustomParameters.atOptional("notOlderThan").value_or("120"));
  mReferenceRun = std::stoi(mCustomParameters.atOptional("referenceRun").value_or("0"));

  // load and initialize the input groups
  for (auto group : mConfig.dataGroups) {
    auto groupName = group.name;
    auto& plotVec = mPlotNames[groupName];
    for (auto path : group.objects) {
      auto fullPath = group.inputPath + "/" + path;
      auto fullRefPath = group.referencePath + "/" + path;
      auto fullOutPath = group.outputPath + "/" + path;

      // retrieve the reference MO
      auto referencePlot = getReferencePlot(qcdb, fullRefPath, t.activity);
      if (!referencePlot) {
        continue;
      }

      // extract the reference histogram
      TH1* referenceHistogram = dynamic_cast<TH1*>(referencePlot->getObject());
      if (!referenceHistogram) {
        continue;
      }

      // store the reference MO
      mReferencePlots[fullPath] = referencePlot;

      // fill an array with the full paths of the plots associated to this group
      plotVec.push_back(fullPath);

      // create and store the plotter object
      mHistograms[fullPath] = std::make_shared<ReferenceComparatorPlot>(referenceHistogram, fullOutPath,
                                                                        group.normalizeReference,
                                                                        group.drawRatioOnly,
                                                                        group.drawOption1D,
                                                                        group.drawOption2D);
      auto* outObject = mHistograms[fullPath]->getMainCanvas();
      // publish the object created by the plotter
      if (outObject) {
        getObjectsManager()->startPublishing(outObject);
      }
    }
  }
}

//_________________________________________________________________________________________

void ReferenceComparatorTask::update(quality_control::postprocessing::Trigger trigger, framework::ServiceRegistryRef services)
{
  auto& qcdb = services.get<repository::DatabaseInterface>();

  for (auto& [groupName, plotVec] : mPlotNames) {
    for (auto& plotName : plotVec) {
      // get object for current timestamp - age limit is converted to milliseconds
      auto object = getMO(qcdb, plotName, trigger, mNotOlderThan * 1000);

      // skip objects that are not found or too old
      if (!object.first || !object.second) {
        continue;
      }

      // only process objects inheriting from TH1
      auto* histogram = dynamic_cast<TH1*>(object.first->getObject());
      if (!histogram) {
        continue;
      }

      // check if a corresponding output plot was initialized
      auto iter = mHistograms.find(plotName);
      if (iter == mHistograms.end()) {
        continue;
      }

      // update the plot ratios and the histograms with superimposed reference
      auto referenceMO = mReferencePlots[plotName];
      TH1* referenceHistogram = dynamic_cast<TH1*>(referenceMO->getObject());

      iter->second->update(histogram, referenceHistogram);
    }
  }
}

//_________________________________________________________________________________________

void ReferenceComparatorTask::finalize(quality_control::postprocessing::Trigger t, framework::ServiceRegistryRef)
{
}

} // namespace o2::quality_control_modules::common
