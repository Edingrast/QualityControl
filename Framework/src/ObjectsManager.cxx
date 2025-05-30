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
/// \file   ObjectsManager.cxx
/// \author Barthelemy von Haller
///

#include "QualityControl/ObjectsManager.h"

#include "QualityControl/QcInfoLogger.h"
#include "QualityControl/MonitorObjectCollection.h"
#include <Common/Exceptions.h>
#include <TObjArray.h>

#include <utility>
#include <algorithm>
#include <ranges>

using namespace o2::quality_control::core;
using namespace AliceO2::Common;
using namespace std;

namespace o2::quality_control::core
{

const std::string ObjectsManager::gDrawOptionsKey = "drawOptions";
const std::string ObjectsManager::gDisplayHintsKey = "displayHints";

ObjectsManager::ObjectsManager(std::string taskName, std::string taskClass, std::string detectorName, int parallelTaskID)
  : mTaskName(std::move(taskName)), mTaskClass(std::move(taskClass)), mDetectorName(std::move(detectorName))
{
  mMonitorObjects = std::make_unique<MonitorObjectCollection>();
  mMonitorObjects->SetOwner(true);
  mMonitorObjects->SetName(mTaskName.c_str());
  mMonitorObjects->setDetector(mDetectorName);
  mMonitorObjects->setTaskName(mTaskName);
}

ObjectsManager::~ObjectsManager()
{
  ILOG(Debug, Devel) << "ObjectsManager destructor" << ENDM;
}

void ObjectsManager::startPublishingImpl(TObject* object, PublicationPolicy publicationPolicy, bool ignoreMergeableWarning)
{
  if (!object) {
    ILOG(Warning, Support) << "A nullptr provided to ObjectManager::startPublishing" << ENDM;
    return;
  }

  if (mMonitorObjects->FindObject(object->GetName()) != nullptr) {
    ILOG(Warning, Support) << "Object is already being published (" << object->GetName() << "), will remove it and add the new one" << ENDM;
    stopPublishing(object->GetName());
  }

  if (!ignoreMergeableWarning && !mergers::isMergeable(object)) {
    ILOG(Warning, Support) << "Object '" + std::string(object->GetName()) + "' with type '" + std::string(object->ClassName()) + "' is not one of the mergeable types, it will not be correctly merged in distributed setups, such as P2 and Grid" << ENDM;
  }

  auto* newObject = new MonitorObject(object, mTaskName, mTaskClass, mDetectorName);
  newObject->setIsOwner(false);
  newObject->setActivity(mActivity);
  newObject->setCreateMovingWindow(std::find(mMovingWindowsList.begin(), mMovingWindowsList.end(), object->GetName()) != mMovingWindowsList.end());
  mMonitorObjects->Add(newObject);
  mPublicationPoliciesForMOs[newObject] = publicationPolicy;
}

void ObjectsManager::stopPublishing(TObject* object)
{
  if (!object) {
    ILOG(Warning, Support) << "A nullptr provided to ObjectManager::stopPublishing" << ENDM;
    return;
  }
  // We look for the MonitorObject which observes the provided object by comparing its address
  // This way, we avoid invoking any methods of the provided object, thus we can stop publishing it even after it is deleted
  MonitorObject* moToRemove = nullptr;
  for (auto moAsTObject : *mMonitorObjects) {
    auto mo = dynamic_cast<MonitorObject*>(moAsTObject);
    if (mo && mo->getObject() == object) {
      moToRemove = mo;
      continue;
    }
  }
  if (moToRemove) {
    mPublicationPoliciesForMOs.erase(moToRemove);
    mMonitorObjects->Remove(moToRemove);
    mMonitorObjects->Compress();
  }
}

void ObjectsManager::stopPublishing(const string& objectName)
{
  auto* mo = dynamic_cast<MonitorObject*>(getMonitorObject(objectName));
  mPublicationPoliciesForMOs.erase(mo);
  mMonitorObjects->Remove(mo);
  mMonitorObjects->Compress();
}

void ObjectsManager::stopPublishing(PublicationPolicy policy)
{
  // we do not use directly the view, because deletions in the policy map would invalidate the iterators in the view
  // c++23 will allow us to do std::ranges::to instead.
  std::vector<MonitorObject*> allObjectsMatchingPolicy;
  for (auto* mo : mPublicationPoliciesForMOs | std::views::filter([policy](const auto& kv) { return kv.second == policy; }) | std::views::keys) {
    allObjectsMatchingPolicy.push_back(mo);
  }

  for (const auto mo : allObjectsMatchingPolicy) {
    stopPublishing(mo->getObject());
  }
}

void ObjectsManager::stopPublishingAll()
{
  mMonitorObjects->Clear();
  mPublicationPoliciesForMOs.clear();
}

bool ObjectsManager::isBeingPublished(const string& name)
{
  return (mMonitorObjects->FindObject(name.c_str()) != nullptr);
}

MonitorObject* ObjectsManager::getMonitorObject(const std::string& objectName)
{
  TObject* object = mMonitorObjects->FindObject(objectName.c_str());
  if (object == nullptr) {
    ILOG(Error, Support) << "ObjectsManager: Unable to find object \"" << objectName << "\"" << ENDM;
    BOOST_THROW_EXCEPTION(ObjectNotFoundError() << errinfo_object_name(objectName));
  }
  return dynamic_cast<MonitorObject*>(object);
}

MonitorObject* ObjectsManager::getMonitorObject(size_t index)
{
  TObject* object = mMonitorObjects->At(index);
  if (object == nullptr) {
    ILOG(Error, Support) << "ObjectsManager: Unable to find object at index \"" << index << "\"" << ENDM;
    string fakeName = "at index " + to_string(index);
    BOOST_THROW_EXCEPTION(ObjectNotFoundError() << errinfo_object_name(fakeName));
  }
  return dynamic_cast<MonitorObject*>(object);
}

MonitorObjectCollection* ObjectsManager::getNonOwningArray() const
{
  return new MonitorObjectCollection(*mMonitorObjects);
}

void ObjectsManager::addMetadata(const std::string& objectName, const std::string& key, const std::string& value)
{
  MonitorObject* mo = getMonitorObject(objectName);
  mo->addMetadata(key, value);
  ILOG(Debug, Devel) << "Added metadata on " << objectName << " : " << key << " -> " << value << ENDM;
}

void ObjectsManager::addOrUpdateMetadata(const std::string& objectName, const std::string& key, const std::string& value)
{
  MonitorObject* mo = getMonitorObject(objectName);
  mo->addOrUpdateMetadata(key, value);
  ILOG(Debug, Devel) << "Added/Modified metadata on " << objectName << " : " << key << " -> " << value << ENDM;
}

size_t ObjectsManager::getNumberPublishedObjects()
{
  return mMonitorObjects->GetLast() + 1; // GetLast returns the index
}

void ObjectsManager::setDefaultDrawOptions(const std::string& objectName, const std::string& options)
{
  MonitorObject* mo = getMonitorObject(objectName);
  mo->addOrUpdateMetadata(gDrawOptionsKey, options);
}

void ObjectsManager::setDefaultDrawOptions(TObject* obj, const std::string& options)
{
  if (!obj) {
    ILOG(Warning, Support) << "A nullptr provided to ObjectManager::setDefaultDrawOptions" << ENDM;
    return;
  }
  MonitorObject* mo = getMonitorObject(obj->GetName());
  mo->addOrUpdateMetadata(gDrawOptionsKey, options);
}

void ObjectsManager::setDisplayHint(const std::string& objectName, const std::string& hints)
{
  MonitorObject* mo = getMonitorObject(objectName);
  mo->addOrUpdateMetadata(gDisplayHintsKey, hints);
}

void ObjectsManager::setDisplayHint(TObject* obj, const std::string& hints)
{
  if (!obj) {
    ILOG(Warning, Support) << "A nullptr provided to ObjectManager::setDisplayHint" << ENDM;
    return;
  }
  MonitorObject* mo = getMonitorObject(obj->GetName());
  mo->addOrUpdateMetadata(gDisplayHintsKey, hints);
}

void ObjectsManager::setValidity(ValidityInterval validityInterval)
{
  for (auto* tobj : *mMonitorObjects) {
    auto* mo = dynamic_cast<MonitorObject*>(tobj);
    if (mo) {
      mo->setValidity(validityInterval);
    } else {
      ILOG(Error, Devel) << "ObjectsManager::setValidity : dynamic_cast returned nullptr." << ENDM;
    }
  }
}

void ObjectsManager::updateValidity(validity_time_t validityTime)
{
  for (auto* tobj : *mMonitorObjects) {
    auto* mo = dynamic_cast<MonitorObject*>(tobj);
    if (mo) {
      mo->updateValidity(validityTime);
    } else {
      ILOG(Error, Devel) << "ObjectsManager::updateValidity : dynamic_cast returned nullptr." << ENDM;
    }
  }
}

const Activity& ObjectsManager::getActivity() const
{
  return mActivity;
}

void ObjectsManager::setActivity(const Activity& activity)
{
  mActivity = activity;
  // update the activity of all the objects
  for (auto tobj : *mMonitorObjects) {
    auto* mo = dynamic_cast<MonitorObject*>(tobj);
    mo->setActivity(activity);
  }
}

void ObjectsManager::setMovingWindowsList(const vector<std::string>& movingWindows)
{
  mMovingWindowsList = movingWindows;
}

const std::vector<std::string>& ObjectsManager::getMovingWindowsList() const
{
  return mMovingWindowsList;
}

} // namespace o2::quality_control::core
