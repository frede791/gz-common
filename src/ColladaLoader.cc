/*
 * Copyright (C) 2015 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */


#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <tinyxml.h>
#include <tinyxml2.h>
#include <math.h>
#include <sstream>
#include <unordered_map>

#include "ignition/math/Helpers.hh"
#include "ignition/math/Angle.hh"
#include "ignition/math/Vector2.hh"
#include "ignition/math/Vector3.hh"
#include "ignition/math/Matrix4.hh"
#include "ignition/math/Quaternion.hh"
#include "ignition/common/CommonTypes.hh"
#include "ignition/common/Console.hh"
#include "ignition/common/Material.hh"
#include "ignition/common/SubMesh.hh"
#include "ignition/common/Mesh.hh"
#include "ignition/common/Skeleton.hh"
#include "ignition/common/SkeletonAnimation.hh"
#include "ignition/common/SystemPaths.hh"
#include "ignition/common/Exception.hh"
#include "ignition/common/ColladaLoaderPrivate.hh"
#include "ignition/common/ColladaLoader.hh"

using namespace ignition;
using namespace common;


/////////////////////////////////////////////////
void hash_combine(std::size_t &_seed, const double &_v)
{
  std::hash<double> hasher;
  _seed ^= hasher(_v) + 0x9e3779b9 + (_seed << 6) + (_seed >> 2);
}

/////////////////////////////////////////////////
struct Vector3Hash : std::unary_function<const ignition::math::Vector3d,
  std::size_t>
{
  std::size_t operator()(const ignition::math::Vector3d &_v) const
  {
    std::size_t seed = 0;
    hash_combine(seed, _v.X());
    hash_combine(seed, _v.Y());
    hash_combine(seed, _v.Z());
    return seed;
  }
};

/////////////////////////////////////////////////
struct Vector2dHash : std::unary_function<const ignition::math::Vector2d,
  std::size_t>
{
  std::size_t operator()(const ignition::math::Vector2d &_v) const
  {
    std::size_t seed = 0;
    hash_combine(seed, _v.X());
    hash_combine(seed, _v.Y());
    return seed;
  }
};

//////////////////////////////////////////////////
  ColladaLoader::ColladaLoader()
: MeshLoader(), dataPtr(new ColladaLoaderPrivate)
{
  this->dataPtr->meter = 1.0;
}

//////////////////////////////////////////////////
ColladaLoader::~ColladaLoader()
{
  delete this->dataPtr;
  this->dataPtr = 0;
}

//////////////////////////////////////////////////
Mesh *ColladaLoader::Load(const std::string &_filename)
{
  this->dataPtr->positionIds.clear();
  this->dataPtr->normalIds.clear();
  this->dataPtr->texcoordIds.clear();
  this->dataPtr->materialIds.clear();
  this->dataPtr->positionDuplicateMap.clear();
  this->dataPtr->normalDuplicateMap.clear();
  this->dataPtr->texcoordDuplicateMap.clear();

  // reset scale
  this->dataPtr->meter = 1.0;

  tinyxml2::XMLDocument xmlDoc;

  this->dataPtr->path.clear();
  if (_filename.rfind('/') != std::string::npos)
  {
    this->dataPtr->path = _filename.substr(0, _filename.rfind('/'));
  }

  this->dataPtr->filename = _filename;
  if (!xmlDoc.LoadFile(_filename.c_str()) != tinyxml2::XML_NO_ERROR)
    ignerr << "Unable to load collada file[" << _filename << "]\n";

  this->dataPtr->colladaXml = xmlDoc.FirstChildElement("COLLADA");
  if (!this->dataPtr->colladaXml)
    ignerr << "Missing COLLADA tag\n";

  if (std::string(this->dataPtr->colladaXml->Attribute("version")) != "1.4.0" &&
      std::string(this->dataPtr->colladaXml->Attribute("version")) != "1.4.1")
    ignerr << "Invalid collada file. Must be version 1.4.0 or 1.4.1\n";

  tinyxml2::XMLElement *assetXml =
      this->dataPtr->colladaXml->FirstChildElement("asset");
  if (assetXml)
  {
    tinyxml2::XMLElement *unitXml = assetXml->FirstChildElement("unit");
    if (unitXml && unitXml->Attribute("meter"))
      this->dataPtr->meter = ignition::math::parseFloat(
          unitXml->Attribute("meter"));
  }

  Mesh *mesh = new Mesh();
  mesh->SetPath(this->dataPtr->path);

  this->LoadScene(mesh);

  // This will make the model the correct size.
  mesh->Scale(ignition::math::Vector3d(
      this->dataPtr->meter, this->dataPtr->meter, this->dataPtr->meter));

  return mesh;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadScene(Mesh *_mesh)
{
  auto *sceneXml = this->dataPtr->colladaXml->FirstChildElement("scene");
  std::string sceneURL =
      sceneXml->FirstChildElement("instance_visual_scene")->Attribute("url");

  tinyxml2::XMLElement *visSceneXml = this->ElementId("visual_scene", sceneURL);

  if (!visSceneXml)
  {
    ignerr << "Unable to find visual_scene id ='" << sceneURL << "'\n";
    return;
  }

  tinyxml2::XMLElement *nodeXml = visSceneXml->FirstChildElement("node");
  while (nodeXml)
  {
    this->LoadNode(nodeXml, _mesh, ignition::math::Matrix4d::Identity);
    nodeXml = nodeXml->NextSiblingElement("node");
  }
}

/////////////////////////////////////////////////
void ColladaLoader::LoadNode(tinyxml2::XMLElement *_elem, Mesh *_mesh,
    const ignition::math::Matrix4d &_transform)
{
  tinyxml2::XMLElement *nodeXml;
  tinyxml2::XMLElement *instGeomXml;

  ignition::math::Matrix4d transform = this->LoadNodeTransform(_elem);
  transform = _transform * transform;

  if (_elem->Attribute("name"))
  {
    this->dataPtr->currentNodeName = _elem->Attribute("name");
  }

  nodeXml = _elem->FirstChildElement("node");
  while (nodeXml)
  {
    this->LoadNode(nodeXml, _mesh, transform);
    nodeXml = nodeXml->NextSiblingElement("node");
  }

  if (_elem->FirstChildElement("instance_node"))
  {
    std::string nodeURLStr =
      _elem->FirstChildElement("instance_node")->Attribute("url");

    nodeXml = this->ElementId("node", nodeURLStr);
    if (!nodeXml)
    {
      ignerr << "Unable to find node[" << nodeURLStr << "]\n";
      return;
    }
    this->LoadNode(nodeXml, _mesh, transform);
    return;
  }
  else
    nodeXml = _elem;

  instGeomXml = nodeXml->FirstChildElement("instance_geometry");
  while (instGeomXml)
  {
    std::string geomURL = instGeomXml->Attribute("url");
    tinyxml2::XMLElement *geomXml = this->ElementId("geometry", geomURL);

    this->dataPtr->materialMap.clear();
    tinyxml2::XMLElement *bindMatXml, *techniqueXml, *matXml;
    bindMatXml = instGeomXml->FirstChildElement("bind_material");
    while (bindMatXml)
    {
      if ((techniqueXml = bindMatXml->FirstChildElement("technique_common")))
      {
        matXml = techniqueXml->FirstChildElement("instance_material");
        while (matXml)
        {
          std::string symbol = matXml->Attribute("symbol");
          std::string target = matXml->Attribute("target");
          this->dataPtr->materialMap[symbol] = target;
          matXml = matXml->NextSiblingElement("instance_material");
        }
      }
      bindMatXml = bindMatXml->NextSiblingElement("bind_material");
    }

    this->LoadGeometry(geomXml, transform, _mesh);
    instGeomXml = instGeomXml->NextSiblingElement("instance_geometry");
  }

  tinyxml2::XMLElement *instContrXml =
    nodeXml->FirstChildElement("instance_controller");
  while (instContrXml)
  {
    std::string contrURL = instContrXml->Attribute("url");
    tinyxml2::XMLElement *contrXml = this->ElementId("controller", contrURL);

    tinyxml2::XMLElement *instSkelXml = instContrXml->FirstChildElement("skeleton");
    std::string rootURL = instSkelXml->GetText();
    tinyxml2::XMLElement *rootNodeXml = this->ElementId("node", rootURL);

    this->dataPtr->materialMap.clear();
    tinyxml2::XMLElement *bindMatXml, *techniqueXml, *matXml;
    bindMatXml = instContrXml->FirstChildElement("bind_material");
    while (bindMatXml)
    {
      if ((techniqueXml = bindMatXml->FirstChildElement("technique_common")))
      {
        matXml = techniqueXml->FirstChildElement("instance_material");
        while (matXml)
        {
          std::string symbol = matXml->Attribute("symbol");
          std::string target = matXml->Attribute("target");
          this->dataPtr->materialMap[symbol] = target;
          matXml = matXml->NextSiblingElement("instance_material");
        }
      }
      bindMatXml = bindMatXml->NextSiblingElement("bind_material");
    }

    this->LoadController(contrXml, rootNodeXml, transform, _mesh);
    instContrXml = instContrXml->NextSiblingElement("instance_controller");
  }
}

/////////////////////////////////////////////////
ignition::math::Matrix4d ColladaLoader::LoadNodeTransform(
    tinyxml2::XMLElement *_elem)
{
  ignition::math::Matrix4d transform(ignition::math::Matrix4d::Identity);

  if (_elem->FirstChildElement("matrix"))
  {
    std::string matrixStr = _elem->FirstChildElement("matrix")->GetText();
    std::istringstream iss(matrixStr);
    std::vector<double> values(16);
    for (unsigned int i = 0; i < 16; ++i)
      iss >> values[i];
    transform.Set(values[0], values[1], values[2], values[3],
        values[4], values[5], values[6], values[7],
        values[8], values[9], values[10], values[11],
        values[12], values[13], values[14], values[15]);
  }
  else
  {
    if (_elem->FirstChildElement("translate"))
    {
      std::string transStr = _elem->FirstChildElement("translate")->GetText();
      ignition::math::Vector3d translate;
      translate = boost::lexical_cast<ignition::math::Vector3d>(transStr);
      // translate *= this->dataPtr->meter;
      transform.Translate(translate);
    }

    tinyxml2::XMLElement *rotateXml = _elem->FirstChildElement("rotate");
    while (rotateXml)
    {
      ignition::math::Matrix3d mat;
      ignition::math::Vector3d axis;
      double angle;

      std::string rotateStr = rotateXml->GetText();
      std::istringstream iss(rotateStr);

      iss >> axis.X() >> axis.Y() >> axis.Z();
      iss >> angle;
      mat.Axis(axis, IGN_DTOR(angle));
      ignition::math::Matrix4d mat4(ignition::math::Matrix4d::Identity);
      mat4 = mat;

      transform = transform * mat4;

      rotateXml = rotateXml->NextSiblingElement("rotate");
    }

    if (_elem->FirstChildElement("scale"))
    {
      std::string scaleStr = _elem->FirstChildElement("scale")->GetText();
      ignition::math::Vector3d scale;
      scale = boost::lexical_cast<ignition::math::Vector3d>(scaleStr);
      ignition::math::Matrix4d scaleMat;
      scaleMat.Scale(scale);
      transform = transform * scaleMat;
    }
  }

  return transform;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadController(tinyxml2::XMLElement *_contrXml,
    tinyxml2::XMLElement *_skelXml, const ignition::math::Matrix4d &_transform,
    Mesh *_mesh)
{
  SkeletonPtr skeleton(new Skeleton(this->LoadSkeletonNodes(_skelXml, NULL)));
  _mesh->SetSkeleton(skeleton);

  tinyxml2::XMLElement *rootXml = _contrXml->GetDocument()->RootElement();

  if (rootXml->FirstChildElement("library_animations"))
    this->LoadAnimations(rootXml->FirstChildElement("library_animations"),
        skeleton);

  tinyxml2::XMLElement *skinXml = _contrXml->FirstChildElement("skin");
  std::string geomURL = skinXml->Attribute("source");

  ignition::math::Matrix4d bindTrans;
  std::string matrixStr =
        skinXml->FirstChildElement("bind_shape_matrix")->GetText();
  std::istringstream iss(matrixStr);
  std::vector<double> values(16);
  for (unsigned int i = 0; i < 16; ++i)
    iss >> values[i];
  bindTrans.Set(values[0], values[1], values[2], values[3],
                values[4], values[5], values[6], values[7],
                values[8], values[9], values[10], values[11],
                values[12], values[13], values[14], values[15]);

  skeleton->SetBindShapeTransform(bindTrans);

  tinyxml2::XMLElement *jointsXml = skinXml->FirstChildElement("joints");
  std::string jointsURL, invBindMatURL;
  tinyxml2::XMLElement *inputXml = jointsXml->FirstChildElement("input");
  while (inputXml)
  {
    std::string semantic = inputXml->Attribute("semantic");
    std::string source = inputXml->Attribute("source");
    if (semantic == "JOINT")
      jointsURL = source;
    else
    {
      if (semantic == "INV_BIND_MATRIX")
        invBindMatURL = source;
    }
    inputXml = inputXml->NextSiblingElement("input");
  }

  jointsXml = this->ElementId("source", jointsURL);

  if (!jointsXml)
  {
    ignerr << "Could not find node[" << jointsURL << "]. "
        << "Faild to parse skinning information in Collada file." << std::endl;
  }

  std::string jointsStr = jointsXml->FirstChildElement("Name_array")->GetText();

  std::vector<std::string> joints;
  boost::split(joints, jointsStr, boost::is_any_of("   "));

  tinyxml2::XMLElement *invBMXml = this->ElementId("source", invBindMatURL);

  if (!invBMXml)
  {
    ignerr << "Could not find node[" << invBindMatURL << "]. "
        << "Faild to parse skinning information in Collada file." << std::endl;
  }

  std::string posesStr = invBMXml->FirstChildElement("float_array")->GetText();

  std::vector<std::string> strs;
  boost::split(strs, posesStr, boost::is_any_of("   "));

  for (unsigned int i = 0; i < joints.size(); ++i)
  {
    unsigned int id = i * 16;
    ignition::math::Matrix4d mat;
    mat.Set(ignition::math::parseFloat(strs[id +  0]),
            ignition::math::parseFloat(strs[id +  1]),
            ignition::math::parseFloat(strs[id +  2]),
            ignition::math::parseFloat(strs[id +  3]),
            ignition::math::parseFloat(strs[id +  4]),
            ignition::math::parseFloat(strs[id +  5]),
            ignition::math::parseFloat(strs[id +  6]),
            ignition::math::parseFloat(strs[id +  7]),
            ignition::math::parseFloat(strs[id +  8]),
            ignition::math::parseFloat(strs[id +  9]),
            ignition::math::parseFloat(strs[id + 10]),
            ignition::math::parseFloat(strs[id + 11]),
            ignition::math::parseFloat(strs[id + 12]),
            ignition::math::parseFloat(strs[id + 13]),
            ignition::math::parseFloat(strs[id + 14]),
            ignition::math::parseFloat(strs[id + 15]));

    skeleton->NodeByName(joints[i])->SetInverseBindTransform(mat);
  }

  tinyxml2::XMLElement *vertWeightsXml =
      skinXml->FirstChildElement("vertex_weights");

  inputXml = vertWeightsXml->FirstChildElement("input");
  unsigned int jOffset = 0;
  unsigned int wOffset = 0;
  std::string weightsURL;
  while (inputXml)
  {
    std::string semantic = inputXml->Attribute("semantic");
    std::string source = inputXml->Attribute("source");
    int offset;
    inputXml->Attribute("offset", &offset);

    if (semantic == "JOINT")
    {
      jOffset = offset;
    }
    else
    {
      if (semantic == "WEIGHT")
      {
        weightsURL = source;
        wOffset = offset;
      }
    }
    inputXml = inputXml->NextSiblingElement("input");
  }

  tinyxml2::XMLElement *weightsXml = this->ElementId("source", weightsURL);

  std::string wString = weightsXml->FirstChildElement("float_array")->GetText();
  std::vector<std::string> wStrs;
  boost::split(wStrs, wString, boost::is_any_of("   "));

  std::vector<float> weights;
  for (unsigned int i = 0; i < wStrs.size(); ++i)
    weights.push_back(math::parseFloat(wStrs[i]));

  std::string cString = vertWeightsXml->FirstChildElement("vcount")->GetText();
  std::string vString = vertWeightsXml->FirstChildElement("v")->GetText();
  std::vector<std::string> vCountStrs;
  std::vector<std::string> vStrs;

  boost::split(vCountStrs, cString, boost::is_any_of("   "));
  boost::split(vStrs, vString, boost::is_any_of("   "));

  std::vector<unsigned int> vCount;
  std::vector<unsigned int> v;

  for (unsigned int i = 0; i < vCountStrs.size(); ++i)
    vCount.push_back(math::parseInt(vCountStrs[i]));

  for (unsigned int i = 0; i < vStrs.size(); ++i)
    v.push_back(math::parseInt(vStrs[i]));

  skeleton->SetNumVertAttached(vCount.size());

  unsigned int vIndex = 0;
  for (unsigned int i = 0; i < vCount.size(); ++i)
  {
    for (unsigned int j = 0; j < vCount[i]; ++j)
    {
      skeleton->AddVertNodeWeight(i, joints[v[vIndex + jOffset]],
                                    weights[v[vIndex + wOffset]]);
      vIndex += (jOffset + wOffset + 1);
    }
  }

  tinyxml2::XMLElement *geomXml = this->ElementId("geometry", geomURL);
  this->LoadGeometry(geomXml, _transform, _mesh);
}

/////////////////////////////////////////////////
void ColladaLoader::LoadAnimations(tinyxml2::XMLElement *_xml,
    SkeletonPtr _skel)
{
  tinyxml2::XMLElement *childXml = _xml->FirstChildElement("animation");
  if (childXml->FirstChildElement("animation"))
  {
    while (childXml)
    {
      this->LoadAnimationSet(childXml, _skel);
      childXml->NextSiblingElement("animation");
    }
  }
  else
    this->LoadAnimationSet(_xml, _skel);
}

/////////////////////////////////////////////////
void ColladaLoader::LoadAnimationSet(tinyxml2::XMLElement *_xml,
    SkeletonPtr _skel)
{
  std::stringstream animName;
  if (_xml->Attribute("name"))
  {
    animName << _xml->Attribute("name");
  }
  else
  {
    if (_xml->Attribute("id"))
      animName << _xml->Attribute("id");
    else
      animName << "animation" << (_skel->AnimationCount() + 1);
  }

  RawSkeletonAnim animation;

  tinyxml2::XMLElement *animXml = _xml->FirstChildElement("animation");
  while (animXml)
  {
    tinyxml2::XMLElement *chanXml = animXml->FirstChildElement("channel");

    while (chanXml)
    {
      std::string sourceURL = chanXml->Attribute("source");
      std::string targetStr = chanXml->Attribute("target");

      std::string targetBone = targetStr.substr(0, targetStr.find('/'));
      char sep = '0';
      if (targetStr.find('(') != std::string::npos)
        sep = '(';
      else
        if (targetStr.find('.') != std::string::npos)
          sep = '.';

      std::string targetTrans;
      if (sep == '0')
        targetTrans = targetStr.substr(targetStr.find('/') + 1);
      else
        targetTrans = targetStr.substr(targetStr.find('/') + 1,
                          targetStr.find(sep) - targetStr.find('/') - 1);

      std::string idxStr = targetStr.substr(targetStr.find(sep) + 1);
      int idx1 = -1;
      int idx2 = -1;

      if (sep == '.')
      {
        idx1 = (idxStr == "X") ? 0 : ((idxStr == "Y") ? 1 : ((idxStr == "Z")
            ? 2 : ((idxStr == "ANGLE") ? 3 : -1)));
      }
      else
      {
        if (sep == '(')
        {
          std::string idx1Str = idxStr.substr(0, 1);
          idx1 = ignition::math::parseInt(idx1Str);
          if (idxStr.length() > 4)
          {
            std::string idx2Str = idxStr.substr(3, 1);
            idx2 = ignition::math::parseInt(idx2Str);
          }
        }
      }

      tinyxml2::XMLElement *frameTimesXml = NULL;
      tinyxml2::XMLElement *frameTransXml = NULL;

      tinyxml2::XMLElement *sampXml = this->ElementId("sampler", sourceURL);
      tinyxml2::XMLElement *inputXml = sampXml->FirstChildElement("input");
      while (inputXml)
      {
        std::string semantic = inputXml->Attribute("semantic");
        if (semantic == "INPUT")
          frameTimesXml = this->ElementId("source",
                              inputXml->Attribute("source"));
        else
          if (semantic == "OUTPUT")
            frameTransXml = this->ElementId("source",
                              inputXml->Attribute("source"));
        /// FIXME interpolation semantic?

        inputXml = inputXml->NextSiblingElement("input");
      }
      tinyxml2::XMLElement *timeArray =
          frameTimesXml->FirstChildElement("float_array");
      std::string timeStr = timeArray->GetText();
      std::vector<std::string> timeStrs;
      boost::split(timeStrs, timeStr, boost::is_any_of("   "));

      std::vector<double> times;
      for (unsigned int i = 0; i < timeStrs.size(); ++i)
        times.push_back(math::parseFloat(timeStrs[i]));

      tinyxml2::XMLElement *output =
          frameTransXml->FirstChildElement("float_array");
      std::string outputStr = output->GetText();
      std::vector<std::string> outputStrs;
      boost::split(outputStrs, outputStr, boost::is_any_of("   "));

      std::vector<double> values;
      for (unsigned int i = 0; i < outputStrs.size(); ++i)
        values.push_back(math::parseFloat(outputStrs[i]));

      tinyxml2::XMLElement *accessor =
        frameTransXml->FirstChildElement("technique_common");
      accessor = accessor->FirstChildElement("accessor");

      unsigned int stride =
        ignition::math::parseInt(accessor->Attribute("stride"));

      for (unsigned int i = 0; i < times.size(); ++i)
      {
        if (animation[targetBone].find(times[i]) == animation[targetBone].end())
          animation[targetBone][times[i]] =
                      _skel->NodeById(targetBone)->Transforms();

        std::vector<NodeTransform> *frame = &animation[targetBone][times[i]];

        for (unsigned int j = 0; j < (*frame).size(); ++j)
        {
          NodeTransform *nt = &((*frame)[j]);
          if (nt->SID() == targetTrans)
          {
            if (idx1 != -1)
            {
              int index = (idx2 == -1) ? idx1 : (idx1 * 4) + idx2;
              nt->SetComponent(index, values[i]);
            }
            else
            {
              for (unsigned int k = 0; k < stride; k++)
                nt->SetComponent(k, values[(i*stride) + k]);
            }
          }
        }
      }

      chanXml = chanXml->NextSiblingElement("channel");
    }

    animXml = animXml->NextSiblingElement("animation");
  }

  SkeletonAnimation *anim = new SkeletonAnimation(animName.str());

  for (RawSkeletonAnim::iterator iter = animation.begin();
        iter != animation.end(); ++iter)
    for (RawNodeAnim::iterator niter = iter->second.begin();
          niter != iter->second.end(); ++niter)
    {
      ignition::math::Matrix4d transform(ignition::math::Matrix4d::Identity);
      for (unsigned int i = 0; i < niter->second.size(); ++i)
      {
        niter->second[i].RecalculateMatrix();
        transform = transform * niter->second[i]();
      }
      anim->AddKeyFrame(iter->first, niter->first, transform);
    }

  _skel->AddAnimation(anim);
}

/////////////////////////////////////////////////
SkeletonNode* ColladaLoader::LoadSkeletonNodes(tinyxml2::XMLElement *_xml,
      SkeletonNode *_parent)
{
  std::string name;
  if (_xml->Attribute("sid"))
    name = _xml->Attribute("sid");
  else
    name = _xml->Attribute("name");

  SkeletonNode* node = new SkeletonNode(_parent, name, _xml->Attribute("id"));

  if (std::string(_xml->Attribute("type")) == std::string("NODE"))
    node->SetType(SkeletonNode::NODE);

  this->SetSkeletonNodeTransform(_xml, node);

  TiXmlElement *childXml = _xml->FirstChildElement("node");
  while (childXml)
  {
    this->LoadSkeletonNodes(childXml, node);
    childXml = childXml->NextSiblingElement("node");
  }
  return node;
}

/////////////////////////////////////////////////
void ColladaLoader::SetSkeletonNodeTransform(tinyxml2::XMLElement *_elem,
      SkeletonNode *_node)
{
  ignition::math::Matrix4d transform(ignition::math::Matrix4d::Identity);

  if (_elem->FirstChildElement("matrix"))
  {
    std::string matrixStr = _elem->FirstChildElement("matrix")->GetText();
    std::istringstream iss(matrixStr);
    std::vector<double> values(16);
    for (unsigned int i = 0; i < 16; ++i)
      iss >> values[i];
    transform.Set(values[0], values[1], values[2], values[3],
        values[4], values[5], values[6], values[7],
        values[8], values[9], values[10], values[11],
        values[12], values[13], values[14], values[15]);

    NodeTransform nt(transform);
    nt.SetSourceValues(transform);
    if (_elem->FirstChildElement("matrix")->Attribute("sid"))
      nt.SetSID(_elem->FirstChildElement("matrix")->Attribute("sid"));
    _node->AddRawTransform(nt);
  }
  else
  {
    if (_elem->FirstChildElement("translate"))
    {
      std::string transStr = _elem->FirstChildElement("translate")->GetText();
      ignition::math::Vector3d translate;
      translate = boost::lexical_cast<ignition::math::Vector3d>(transStr);
      // translate *= this->dataPtr->meter;
      transform.Translate(translate);

      NodeTransform nt(transform);
      if (_elem->FirstChildElement("translate")->Attribute("sid"))
        nt.SetSID(_elem->FirstChildElement("translate")->Attribute("sid"));
      nt.SetType(NodeTransformType::TRANSLATE);
      nt.SetSourceValues(translate);
      _node->AddRawTransform(nt);
    }

    tinyxml2::XMLElement *rotateXml = _elem->FirstChildElement("rotate");
    while (rotateXml)
    {
      ignition::math::Matrix3d mat;
      ignition::math::Vector3d axis;
      double angle;

      std::string rotateStr = rotateXml->GetText();
      std::istringstream iss(rotateStr);

      iss >> axis.X() >> axis.Y() >> axis.Z();
      iss >> angle;
      mat.Axis(axis, IGN_DTOR(angle));

      ignition::math::Matrix4d mat4(ignition::math::Matrix4d::Identity);
      mat4 = mat;
      NodeTransform nt(mat4);
      if (rotateXml->Attribute("sid"))
        nt.SetSID(rotateXml->Attribute("sid"));
      nt.SetType(NodeTransformType::ROTATE);
      nt.SetSourceValues(axis, angle);
      _node->AddRawTransform(nt);

      transform = transform * mat4;

      rotateXml = rotateXml->NextSiblingElement("rotate");
    }

    if (_elem->FirstChildElement("scale"))
    {
      std::string scaleStr = _elem->FirstChildElement("scale")->GetText();
      ignition::math::Vector3d scale;
      scale = boost::lexical_cast<ignition::math::Vector3d>(scaleStr);
      ignition::math::Matrix4d scaleMat;
      scaleMat.Scale(scale);

      NodeTransform nt(scaleMat);
      if (_elem->FirstChildElement("matrix")->Attribute("sid"))
        nt.SetSID(_elem->FirstChildElement("matrix")->Attribute("sid"));
      nt.SetType(NodeTransformType::SCALE);
      nt.SetSourceValues(scale);
      _node->AddRawTransform(nt);

      transform = transform * scaleMat;
    }
  }

  _node->SetTransform(transform);
}

/////////////////////////////////////////////////
void ColladaLoader::LoadGeometry(tinyxml2::XMLElement *_xml,
    const ignition::math::Matrix4d &_transform, Mesh *_mesh)
{
  tinyxml2::XMLElement *meshXml = _xml->FirstChildElement("mesh");
  tinyxml2::XMLElement *childXml;

  if (!meshXml)
    return;

  childXml = meshXml->FirstChildElement("triangles");
  while (childXml)
  {
    this->LoadTriangles(childXml, _transform, _mesh);
    childXml = childXml->NextSiblingElement("triangles");
  }

  childXml = meshXml->FirstChildElement("polylist");
  while (childXml)
  {
    this->LoadPolylist(childXml, _transform, _mesh);
    childXml = childXml->NextSiblingElement("polylist");
  }

  childXml = meshXml->FirstChildElement("lines");
  while (childXml)
  {
    this->LoadLines(childXml, _transform, _mesh);
    childXml = childXml->NextSiblingElement("lines");
  }
}

/////////////////////////////////////////////////
tinyxml2::XMLElement *ColladaLoader::ElementId(const std::string &_name,
    const std::string &_id)
{
  return this->ElementId(this->dataPtr->colladaXml, _name, _id);
}

/////////////////////////////////////////////////
tinyxml2::XMLElement *ColladaLoader::ElementId(tinyxml2::XMLElement *_parent,
    const std::string &_name, const std::string &_id)
{
  std::string id = _id;
  if (id.length() > 0 && id[0] == '#')
    id.erase(0, 1);

  if ((id.empty() && _parent->Value() == _name) ||
      (_parent->Attribute("id") && _parent->Attribute("id") == id) ||
      (_parent->Attribute("sid") && _parent->Attribute("sid") == id))
  {
    return _parent;
  }

  tinyxml2::XMLElement *elem = _parent->FirstChildElement();
  while (elem)
  {
    tinyxml2::XMLElement *result = this->ElementId(elem, _name, _id);
    if (result)
    {
      return result;
    }

    elem = elem->NextSiblingElement();
  }

  return NULL;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadVertices(const std::string &_id,
    const ignition::math::Matrix4d &_transform,
    std::vector<ignition::math::Vector3d> &_verts,
    std::vector<ignition::math::Vector3d> &_norms)
{
  std::map<unsigned int, unsigned int> vertDup;
  std::map<unsigned int, unsigned int> normDup;
  this->LoadVertices(_id, _transform, _verts, _norms, vertDup, normDup);
}

/////////////////////////////////////////////////
void ColladaLoader::LoadVertices(const std::string &_id,
    const ignition::math::Matrix4d &_transform,
    std::vector<ignition::math::Vector3d> &_verts,
    std::vector<ignition::math::Vector3d> &_norms,
    std::map<unsigned int, unsigned int> &_vertDups,
    std::map<unsigned int, unsigned int> &_normDups)
{
  tinyxml2::XMLElement *verticesXml = this->ElementId(this->dataPtr->colladaXml,
      "vertices", _id);

  if (!verticesXml)
  {
    ignerr << "Unable to find vertices[" << _id << "] in collada file\n";
    return;
  }

  tinyxml2::XMLElement *inputXml = verticesXml->FirstChildElement("input");
  while (inputXml)
  {
    std::string semantic = inputXml->Attribute("semantic");
    std::string sourceStr = inputXml->Attribute("source");
    if (semantic == "NORMAL")
    {
      this->LoadNormals(sourceStr, _transform, _norms, _normDups);
    }
    else if (semantic == "POSITION")
    {
      this->LoadPositions(sourceStr, _transform, _verts, _vertDups);
    }

    inputXml = inputXml->NextSiblingElement("input");
  }
}

/////////////////////////////////////////////////
void ColladaLoader::LoadPositions(const std::string &_id,
    const ignition::math::Matrix4d &_transform,
    std::vector<ignition::math::Vector3d> &_values,
    std::map<unsigned int, unsigned int> &_duplicates)
{
  if (this->dataPtr->positionIds.find(_id) != this->dataPtr->positionIds.end())
  {
    _values = this->dataPtr->positionIds[_id];
    _duplicates = this->dataPtr->positionDuplicateMap[_id];
    return;
  }

  tinyxml2::XMLElement *sourceXml = this->ElementId("source", _id);
  if (!sourceXml)
  {
    ignerr << "Unable to find source\n";
    return;
  }

  tinyxml2::XMLElement *floatArrayXml =
      sourceXml->FirstChildElement("float_array");
  if (!floatArrayXml || !floatArrayXml->GetText())
  {
    int count = 1;
    if (floatArrayXml && floatArrayXml->Attribute("count"))
    {
      try
      {
        count = boost::lexical_cast<int>(floatArrayXml->Attribute("count"));
      }
      catch(...)
      {
        // Do nothing. Messages are printed out below.
      }
    }

    if (count)
    {
      ignerr << "Vertex source missing float_array element, "
        << "or count is invalid.\n";
    }
    else
    {
      ignlog << "Vertex source has a float_array with a count of zero. "
        << "This is likely not desired\n";
    }

    return;
  }
  std::string valueStr = floatArrayXml->GetText();

  std::unordered_map<ignition::math::Vector3d,
      unsigned int, Vector3Hash> unique;

  std::vector<std::string> strs;
  std::vector<std::string>::iterator iter, end;
  boost::split(strs, valueStr, boost::is_any_of("   "));

  end = strs.end();
  for (iter = strs.begin(); iter != end; iter += 3)
  {
    ignition::math::Vector3d vec(math::parseFloat(*iter),
        ignition::math::parseFloat(*(iter+1)),
        ignition::math::parseFloat(*(iter+2)));

    vec = _transform * vec;
    _values.push_back(vec);

    // create a map of duplicate indices
    if (unique.find(vec) != unique.end())
      _duplicates[_values.size()-1] = unique[vec];
    else
      unique[vec] = _values.size()-1;
  }

  this->dataPtr->positionDuplicateMap[_id] = _duplicates;
  this->dataPtr->positionIds[_id] = _values;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadNormals(const std::string &_id,
    const ignition::math::Matrix4d &_transform,
    std::vector<ignition::math::Vector3d> &_values,
    std::map<unsigned int, unsigned int> &_duplicates)
{
  if (this->dataPtr->normalIds.find(_id) != this->dataPtr->normalIds.end())
  {
    _values = this->dataPtr->normalIds[_id];
    _duplicates = this->dataPtr->normalDuplicateMap[_id];
    return;
  }

  ignition::math::Matrix4d rotMat = _transform;
  rotMat.Translate(ignition::math::Vector3d::Zero);

  tinyxml2::XMLElement *normalsXml = this->ElementId("source", _id);
  if (!normalsXml)
  {
    ignerr << "Unable to find normals[" << _id << "] in collada file\n";
    return;
  }

  tinyxml2::XMLElement *floatArrayXml =
      normalsXml->FirstChildElement("float_array");
  if (!floatArrayXml || !floatArrayXml->GetText())
  {
    int count = 1;
    if (floatArrayXml && floatArrayXml->Attribute("count"))
    {
      try
      {
        count = boost::lexical_cast<int>(floatArrayXml->Attribute("count"));
      }
      catch(...)
      {
        // Do nothing. Messages are printed out below.
      }
    }

    if (count)
    {
      ignwarn << "Normal source missing float_array element, or count is "
        << "invalid.\n";
    }
    else
    {
      ignlog << "Normal source has a float_array with a count of zero. "
        << "This is likely not desired\n";
    }

    return;
  }

  std::unordered_map<ignition::math::Vector3d,
      unsigned int, Vector3Hash> unique;

  std::string valueStr = floatArrayXml->GetText();
  std::istringstream iss(valueStr);
  do
  {
    ignition::math::Vector3d vec;
    iss >> vec.X() >> vec.Y() >> vec.Z();
    if (iss)
    {
      vec = rotMat * vec;
      vec.Normalize();
      _values.push_back(vec);

      // create a map of duplicate indices
      if (unique.find(vec) != unique.end())
        _duplicates[_values.size()-1] = unique[vec];
      else
        unique[vec] = _values.size()-1;
    }
  } while (iss);

  this->dataPtr->normalDuplicateMap[_id] = _duplicates;
  this->dataPtr->normalIds[_id] = _values;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadTexCoords(const std::string &_id,
    std::vector<ignition::math::Vector2d> &_values,
    std::map<unsigned int, unsigned int> &_duplicates)
{
  if (this->dataPtr->texcoordIds.find(_id) != this->dataPtr->texcoordIds.end())
  {
    _values = this->dataPtr->texcoordIds[_id];
    _duplicates = this->dataPtr->texcoordDuplicateMap[_id];
    return;
  }

  int stride = 0;
  int texCount = 0;
  int totCount = 0;

  // Get the source element for the texture coordinates.
  tinyxml2::XMLElement *xml = this->ElementId("source", _id);
  if (!xml)
  {
    ignerr << "Unable to find tex coords[" << _id << "] in collada file\n";
    return;
  }

  // Get the array of float values. These are the raw values for the texture
  // coordinates.
  tinyxml2::XMLElement *floatArrayXml = xml->FirstChildElement("float_array");
  if (!floatArrayXml || !floatArrayXml->GetText())
  {
    int count = 1;
    if (floatArrayXml && floatArrayXml->Attribute("count"))
    {
      try
      {
        count = boost::lexical_cast<int>(floatArrayXml->Attribute("count"));
      }
      catch(...)
      {
        // Do nothing. Messages are printed out below.
      }
    }

    if (count)
    {
      ignerr << "Normal source missing float_array element, or count is "
        << "invalid.\n";
    }
    else
    {
      ignlog << "Normal source has a float_array with a count of zero. "
        << "This is likely not desired\n";
    }

    return;
  }
  // Read in the total number of texture coordinate values
  else if (floatArrayXml->Attribute("count"))
    totCount = boost::lexical_cast<int>(floatArrayXml->Attribute("count"));
  else
  {
    ignerr << "<float_array> has no count attribute in texture coordinate "
          << "element with id[" << _id << "]\n";
    return;
  }

  // The technique_common holds an <accessor> element that indicates how to
  // parse the float array.
  xml = xml->FirstChildElement("technique_common");
  if (!xml)
  {
    ignerr << "Unable to find technique_common element for texture "
          << "coordinates with id[" << _id << "]\n";
    return;
  }

  // Get the accessor XML element.
  xml = xml->FirstChildElement("accessor");
  if (!xml)
  {
    ignerr << "Unable to find <accessor> as a child of <technique_common> "
          << "for texture coordinates with id[" << _id << "]\n";
    return;
  }

  // Read in the stride for the texture coordinate values. The stride
  // indicates the number of values in the float array the comprise
  // a complete texture coordinate.
  if (xml->Attribute("stride"))
  {
    stride = boost::lexical_cast<int>(xml->Attribute("stride"));
  }
  else
  {
    ignerr << "<accessor> has no stride attribute in texture coordinate "
          << "element with id[" << _id << "]\n";
    return;
  }

  // Read in the count of texture coordinates.
  if (xml->Attribute("count"))
    texCount = boost::lexical_cast<int>(xml->Attribute("count"));
  else
  {
    ignerr << "<accessor> has no count attribute in texture coordinate element "
          << "with id[" << _id << "]\n";
    return;
  }

  // \TODO This is a good a IGN_ASSERT
  // The total number of texture values should equal the stride multiplied
  // by the number of texture coordinates.
  if (texCount * stride != totCount)
  {
    ignerr << "Error reading texture coordinates. Coordinate counts in element "
             "with id[" << _id << "] do not add up correctly\n";
    return;
  }

  // Nothing to read. Don't print a warning because the collada file is
  // correct.
  if (totCount == 0)
    return;

  std::unordered_map<ignition::math::Vector2d,
      unsigned int, Vector2dHash> unique;

  // Read the raw texture values, and split them on spaces.
  std::string valueStr = floatArrayXml->GetText();
  std::vector<std::string> values;
  boost::split(values, valueStr, boost::is_any_of(" "));

  // Read in all the texture coordinates.
  for (int i = 0; i < totCount; i += stride)
  {
    // We only handle 2D texture coordinates right now.
    ignition::math::Vector2d vec(boost::lexical_cast<double>(values[i]),
          1.0 - boost::lexical_cast<double>(values[i+1]));
    _values.push_back(vec);

    // create a map of duplicate indices
    if (unique.find(vec) != unique.end())
    {
      _duplicates[_values.size()-1] = unique[vec];
    }
    else
      unique[vec] = _values.size()-1;
  }

  this->dataPtr->texcoordDuplicateMap[_id] = _duplicates;
  this->dataPtr->texcoordIds[_id] = _values;
}

/////////////////////////////////////////////////
MaterialPtr ColladaLoader::LoadMaterial(const std::string &_name)
{
  if (this->dataPtr->materialIds.find(_name)
      != this->dataPtr->materialIds.end())
  {
    return this->dataPtr->materialIds[_name];
  }

  tinyxml2::XMLElement *matXml = this->ElementId("material", _name);
  if (!matXml || !matXml->FirstChildElement("instance_effect"))
    return NULL;

  MaterialPtr mat(new Material());
  std::string effectName =
    matXml->FirstChildElement("instance_effect")->Attribute("url");
  tinyxml2::XMLElement *effectXml = this->ElementId("effect", effectName);

  tinyxml2::XMLElement *commonXml =
     effectXml->FirstChildElement("profile_COMMON");
  if (commonXml)
  {
    tinyxml2::XMLElement *techniqueXml =
       commonXml->FirstChildElement("technique");
    tinyxml2::XMLElement *lambertXml =
       techniqueXml->FirstChildElement("lambert");

    tinyxml2::XMLElement *phongXml = techniqueXml->FirstChildElement("phong");
    tinyxml2::XMLElement *blinnXml = techniqueXml->FirstChildElement("blinn");
    if (lambertXml)
    {
      this->LoadColorOrTexture(lambertXml, "ambient", mat);
      this->LoadColorOrTexture(lambertXml, "emission", mat);
      this->LoadColorOrTexture(lambertXml, "diffuse", mat);
      if (lambertXml->FirstChildElement("transparency"))
      {
        mat->SetTransparency(
            this->LoadFloat(lambertXml->FirstChildElement("transparency")));
      }

      if (lambertXml->FirstChildElement("transparent"))
      {
        TiXmlElement *transXml = lambertXml->FirstChildElement("transparent");
        this->LoadTransparent(transXml, mat);
      }
    }
    else if (phongXml)
    {
      this->LoadColorOrTexture(phongXml, "ambient", mat);
      this->LoadColorOrTexture(phongXml, "emission", mat);
      this->LoadColorOrTexture(phongXml, "specular", mat);
      this->LoadColorOrTexture(phongXml, "diffuse", mat);
      if (phongXml->FirstChildElement("shininess"))
        mat->SetShininess(
            this->LoadFloat(phongXml->FirstChildElement("shininess")));

      if (phongXml->FirstChildElement("transparency"))
        mat->SetTransparency(
            this->LoadFloat(phongXml->FirstChildElement("transparency")));
      if (phongXml->FirstChildElement("transparent"))
      {
        tinyxml2::XMLElement *transXml =
            phongXml->FirstChildElement("transparent");
        this->LoadTransparent(transXml, mat);
      }
    }
    else if (blinnXml)
    {
      this->LoadColorOrTexture(blinnXml, "ambient", mat);
      this->LoadColorOrTexture(blinnXml, "emission", mat);
      this->LoadColorOrTexture(blinnXml, "specular", mat);
      this->LoadColorOrTexture(blinnXml, "diffuse", mat);
      if (blinnXml->FirstChildElement("shininess"))
        mat->SetShininess(
            this->LoadFloat(blinnXml->FirstChildElement("shininess")));

      if (blinnXml->FirstChildElement("transparency"))
        mat->SetTransparency(
            this->LoadFloat(blinnXml->FirstChildElement("transparency")));
      if (blinnXml->FirstChildElement("transparent"))
      {
        tinyxml2::XMLElement *transXml =
            blinnXml->FirstChildElement("transparent");
        this->LoadTransparent(transXml, mat);
      }
    }
  }

  tinyxml2::XMLElement *glslXml = effectXml->FirstChildElement("profile_GLSL");
  if (glslXml)
    ignerr << "profile_GLSL unsupported\n";

  tinyxml2::XMLElement *cgXml = effectXml->FirstChildElement("profile_CG");
  if (cgXml)
    ignerr << "profile_CG unsupported\n";

  this->dataPtr->materialIds[_name] = mat;

  return mat;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadColorOrTexture(tinyxml2::XMLElement *_elem,
    const std::string &_type, MaterialPtr _mat)
{
  if (!_elem || !_elem->FirstChildElement(_type))
    return;

  tinyxml2::XMLElement *typeElem = _elem->FirstChildElement(_type);

  if (typeElem->FirstChildElement("color"))
  {
    std::string colorStr = typeElem->FirstChildElement("color")->GetText();
    Color color = boost::lexical_cast<Color>(colorStr);
    if (_type == "diffuse")
      _mat->SetDiffuse(color);
    else if (_type == "ambient")
      _mat->SetAmbient(color);
    else if (_type == "emission")
      _mat->SetEmissive(color);
    else if (_type == "specular")
      _mat->SetSpecular(color);
  }
  else if (typeElem->FirstChildElement("texture"))
  {
    _mat->SetLighting(true);
    tinyxml2::XMLElement *imageXml = NULL;
    std::string textureName =
      typeElem->FirstChildElement("texture")->Attribute("texture");
    tinyxml2::XMLElement *textureXml = this->ElementId("newparam", textureName);
    if (textureXml)
    {
      if (std::string(textureXml->Value()) == "image")
      {
        imageXml = textureXml;
      }
      else
      {
        tinyxml2::XMLElement *sampler =
            textureXml->FirstChildElement("sampler2D");
        if (sampler)
        {
          std::string sourceName =
              sampler->FirstChildElement("source")->GetText();
          tinyxml2::XMLElement *sourceXml =
              this->ElementId("newparam", sourceName);
          if (sourceXml)
          {
            tinyxml2::XMLElement *surfaceXml =
                sourceXml->FirstChildElement("surface");
            if (surfaceXml && surfaceXml->FirstChildElement("init_from"))
            {
              imageXml = this->ElementId("image",
                  surfaceXml->FirstChildElement("init_from")->GetText());
            }
          }
        }
      }
    }
    else
    {
      imageXml = this->ElementId("image", textureName);
    }

    if (imageXml && imageXml->FirstChildElement("init_from"))
    {
      std::string imgFile =
        imageXml->FirstChildElement("init_from")->GetText();
      _mat->SetTextureImage(imgFile, this->dataPtr->path);
    }
  }
}

/////////////////////////////////////////////////
void ColladaLoader::LoadPolylist(tinyxml2::XMLElement *_polylistXml,
    const ignition::math::Matrix4d &_transform,
    Mesh *_mesh)
{
  // This function parses polylist types in collada into
  // a set of triangle meshes.  The assumption is that
  // each polylist polygon is convex, and we do decomposion
  // by anchoring each triangle about vertex 0 or each polygon
  SubMeshPtr subMesh(new SubMesh());
  subMesh->SetName(this->dataPtr->currentNodeName);
  bool combinedVertNorms = false;

  subMesh->SetPrimitiveType(SubMesh::TRIANGLES);

  if (_polylistXml->Attribute("material"))
  {
    std::map<std::string, std::string>::iterator iter;
    std::string matStr = _polylistXml->Attribute("material");

    int matIndex = -1;
    iter = this->dataPtr->materialMap.find(matStr);
    if (iter != this->dataPtr->materialMap.end())
      matStr = iter->second;

    MaterialPtr mat = this->LoadMaterial(matStr);

    matIndex = _mesh->IndexOfMaterial(mat);
    if (matIndex < 0)
      matIndex = _mesh->AddMaterial(mat);

    if (matIndex < 0)
      ignwarn << "Unable to add material[" << matStr << "]\n";
    else
      subMesh->SetMaterialIndex(matIndex);
  }

  tinyxml2::XMLElement *polylistInputXml =
      _polylistXml->FirstChildElement("input");

  std::vector<ignition::math::Vector3d> verts;
  std::vector<ignition::math::Vector3d> norms;
  std::vector<ignition::math::Vector2d> texcoords;

  const unsigned int VERTEX = 0;
  const unsigned int NORMAL = 1;
  const unsigned int TEXCOORD = 2;
  unsigned int otherSemantics = TEXCOORD + 1;
  bool hasVertices = false;
  bool hasNormals = false;
  bool hasTexcoords = false;

  // look up table of position/normal/texcoord duplicate indices
  std::map<unsigned int, unsigned int> texDupMap;
  std::map<unsigned int, unsigned int> normalDupMap;
  std::map<unsigned int, unsigned int> positionDupMap;

  ignition::math::Matrix4d bindShapeMat(ignition::math::Matrix4d::Identity);
  if (_mesh->HasSkeleton())
    bindShapeMat = _mesh->MeshSkeleton()->BindShapeTransform();

  // read input elements
  std::map<const unsigned int, int> inputs;
  while (polylistInputXml)
  {
    std::string semantic = polylistInputXml->Attribute("semantic");
    std::string source = polylistInputXml->Attribute("source");
    std::string offset = polylistInputXml->Attribute("offset");
    if (semantic == "VERTEX")
    {
      unsigned int count = norms.size();
      this->LoadVertices(source, _transform, verts, norms,
          positionDupMap, normalDupMap);
      if (norms.size() > count)
        combinedVertNorms = true;
      inputs[VERTEX] = ignition::math::parseInt(offset);
      hasVertices = true;
    }
    else if (semantic == "NORMAL")
    {
      this->LoadNormals(source, _transform, norms, normalDupMap);
      combinedVertNorms = false;
      inputs[NORMAL] = ignition::math::parseInt(offset);
      hasNormals = true;
    }
    else if (semantic == "TEXCOORD")
    {
      this->LoadTexCoords(source, texcoords, texDupMap);
      inputs[TEXCOORD] = ignition::math::parseInt(offset);
      hasTexcoords = true;
    }
    else
    {
      inputs[otherSemantics++] = ignition::math::parseInt(offset);
      ignwarn << "Polylist input semantic: '" << semantic << "' is currently"
          << " not supported" << std::endl;
    }

    polylistInputXml = polylistInputXml->NextSiblingElement("input");
  }

  // read vcount
  // break poly into triangles
  // if vcount >= 4, anchor around 0 (note this is bad for concave elements)
  //   e.g. if vcount = 4, break into triangle 1: [0,1,2], triangle 2: [0,2,3]
  std::vector<std::string> vcountStrs;
  tinyxml2::XMLElement *vcountXml = _polylistXml->FirstChildElement("vcount");
  std::string vcountStr = vcountXml->GetText();
  boost::split(vcountStrs, vcountStr, boost::is_any_of("   "));
  std::vector<int> vcounts;
  for (unsigned int j = 0; j < vcountStrs.size(); ++j)
    vcounts.push_back(math::parseInt(vcountStrs[j]));

  // read p
  tinyxml2::XMLElement *pXml = _polylistXml->FirstChildElement("p");
  std::string pStr = pXml->GetText();

  // vertexIndexMap is a map of collada vertex index to Gazebo submesh vertex
  // indices, used for identifying vertices that can be shared.
  std::map<unsigned int, std::vector<GeometryIndices> > vertexIndexMap;
  unsigned int *values = new unsigned int[inputs.size()];

  std::vector<std::string> strs;
  boost::split(strs, pStr, boost::is_any_of("   "));
  std::vector<std::string>::iterator strs_iter = strs.begin();
  for (unsigned int l = 0; l < vcounts.size(); ++l)
  {
    // put us at the beginning of the polygon list
    if (l > 0) strs_iter += inputs.size()*vcounts[l-1];

    for (unsigned int k = 2; k < (unsigned int)vcounts[l]; ++k)
    {
      // if vcounts[l] = 5, then read 0,1,2, then 0,2,3, 0,3,4,...
      // here k = the last number in the series
      // j is the triangle loop
      for (unsigned int j = 0; j < 3; ++j)
      {
        // break polygon into triangles
        unsigned int triangle_index;

        if (j == 0)
          triangle_index = 0;
        if (j == 1)
          triangle_index = (k-1)*inputs.size();
        if (j == 2)
          triangle_index = (k)*inputs.size();

        for (unsigned int i = 0; i < inputs.size(); ++i)
        {
          values[i] = ignition::math::parseInt(strs_iter[triangle_index+i]);
          /*ignerr << "debug parsing "
                << " poly-i[" << l
                << "] tri-end-index[" << k
                << "] tri-vertex-i[" << j
                << "] triangle[" << triangle_index
                << "] input[" << i
                << "] value[" << values[i]
                << "]\n"; */
        }


        unsigned int daeVertIndex = 0;
        bool addIndex = !hasVertices;

        // find a set of vertex/normal/texcoord that can be reused
        // only do this if the mesh has vertices
        if (hasVertices)
        {
          // Get the vertex position index value. If it is a duplicate then use
          // the existing index instead
          daeVertIndex = values[inputs[VERTEX]];
          if (positionDupMap.find(daeVertIndex) != positionDupMap.end())
            daeVertIndex = positionDupMap[daeVertIndex];

          // if the vertex index has not been previously added then just add it.
          if (vertexIndexMap.find(daeVertIndex) == vertexIndexMap.end())
          {
            addIndex = true;
          }
          else
          {
            // if the vertex index was previously added, check to see if it has
            // the same normal and texcoord index values
            bool toDuplicate = true;
            unsigned int reuseIndex = 0;
            std::vector<GeometryIndices> inputValues =
                vertexIndexMap[daeVertIndex];

            for (unsigned int i = 0; i < inputValues.size(); ++i)
            {
              GeometryIndices iv = inputValues[i];
              bool normEqual = false;
              bool texEqual = false;

              if (hasNormals)
              {
                // Get the vertex normal index value. If the normal is a
                // duplicate then reset the index to the first instance of the
                // duplicated position
                unsigned int remappedNormalIndex = values[inputs[NORMAL]];
                if (normalDupMap.find(remappedNormalIndex)
                    != normalDupMap.end())
                 {
                  remappedNormalIndex = normalDupMap[remappedNormalIndex];
                 }

                if (iv.normalIndex == remappedNormalIndex)
                  normEqual = true;
              }
              if (hasTexcoords)
              {
                // Get the vertex texcoord index value. If the texcoord is a
                // duplicate then reset the index to the first instance of the
                // duplicated texcoord
                unsigned int remappedTexcoordIndex = values[inputs[TEXCOORD]];
                if (texDupMap.find(remappedTexcoordIndex) != texDupMap.end())
                  remappedTexcoordIndex = texDupMap[remappedTexcoordIndex];

                if (iv.texcoordIndex == remappedTexcoordIndex)
                  texEqual = true;
              }

              // if the vertex has matching normal and texcoord index values
              // then the vertex can be reused.
              if ((!hasNormals || normEqual) && (!hasTexcoords || texEqual))
              {
                // found a vertex that can be shared.
                toDuplicate = false;
                reuseIndex = iv.mappedIndex;
                subMesh->AddIndex(reuseIndex);
                break;
              }
            }
            addIndex = toDuplicate;
          }
        }

        // if the vertex index is new or can not be shared then add it
        if (addIndex)
        {
          GeometryIndices input;
          if (hasVertices)
          {
            subMesh->AddVertex(verts[daeVertIndex]);
            unsigned int newVertIndex = subMesh->VertexCount()-1;
            subMesh->AddIndex(newVertIndex);
            if (combinedVertNorms)
              subMesh->AddNormal(norms[daeVertIndex]);
            if (_mesh->HasSkeleton())
            {
              subMesh->SetVertex(newVertIndex, bindShapeMat *
                  subMesh->Vertex(newVertIndex));
              SkeletonPtr skel = _mesh->MeshSkeleton();
              for (unsigned int i = 0;
                  i < skel->VertNodeWeightCount(daeVertIndex); ++i)
              {
                std::pair<std::string, double> node_weight =
                  skel->VertNodeWeight(daeVertIndex, i);
                SkeletonNode *node =
                    _mesh->MeshSkeleton()->NodeByName(node_weight.first);
                subMesh->AddNodeAssignment(subMesh->VertexCount()-1,
                                node->Handle(), node_weight.second);
              }
            }
            input.vertexIndex = daeVertIndex;
            input.mappedIndex = newVertIndex;
          }
          if (hasNormals)
          {
            unsigned int inputRemappedNormalIndex = values[inputs[NORMAL]];
            if (normalDupMap.find(inputRemappedNormalIndex)
                != normalDupMap.end())
              inputRemappedNormalIndex = normalDupMap[inputRemappedNormalIndex];
            subMesh->AddNormal(norms[inputRemappedNormalIndex]);
            input.normalIndex = inputRemappedNormalIndex;
          }
          if (hasTexcoords)
          {
            unsigned int inputRemappedTexcoordIndex = values[inputs[TEXCOORD]];
            if (texDupMap.find(inputRemappedTexcoordIndex) != texDupMap.end())
            {
              inputRemappedTexcoordIndex =
                  texDupMap[inputRemappedTexcoordIndex];
            }
            subMesh->AddTexCoord(texcoords[inputRemappedTexcoordIndex].X(),
                texcoords[inputRemappedTexcoordIndex].Y());
            input.texcoordIndex = inputRemappedTexcoordIndex;
          }

          // add the new ignition submesh vertex index to the map
          if (hasVertices)
          {
            std::vector<GeometryIndices> inputValues;
            inputValues.push_back(input);
            vertexIndexMap[daeVertIndex] = inputValues;
          }
        }
      }
    }
  }
  delete [] values;

  _mesh->AddSubMesh(subMesh);
}

/////////////////////////////////////////////////
void ColladaLoader::LoadTriangles(tinyxml2::XMLElement *_trianglesXml,
                                  const ignition::math::Matrix4d &_transform,
                                  Mesh *_mesh)
{
  SubMeshPtr subMesh(new SubMesh);
  subMesh->SetName(this->dataPtr->currentNodeName);
  bool combinedVertNorms = false;

  subMesh->SetPrimitiveType(SubMesh::TRIANGLES);

  if (_trianglesXml->Attribute("material"))
  {
    std::map<std::string, std::string>::iterator iter;
    std::string matStr = _trianglesXml->Attribute("material");

    int matIndex = -1;
    iter = this->dataPtr->materialMap.find(matStr);
    if (iter != this->dataPtr->materialMap.end())
      matStr = iter->second;

    MaterialPtr mat = this->LoadMaterial(matStr);
    matIndex = _mesh->IndexOfMaterial(mat);
    if (matIndex < 0)
      matIndex = _mesh->AddMaterial(mat);

    if (matIndex < 0)
      ignwarn << "Unable to add material[" << matStr << "]\n";
    else
      subMesh->SetMaterialIndex(matIndex);
  }

  tinyxml2::XMLElement *trianglesInputXml =
      _trianglesXml->FirstChildElement("input");

  std::vector<ignition::math::Vector3d> verts;
  std::vector<ignition::math::Vector3d> norms;
  std::vector<ignition::math::Vector2d> texcoords;

  const unsigned int VERTEX = 0;
  const unsigned int NORMAL = 1;
  const unsigned int TEXCOORD = 2;
  unsigned int otherSemantics = TEXCOORD + 1;
  bool hasVertices = false;
  bool hasNormals = false;
  bool hasTexcoords = false;
  unsigned int offsetSize = 0;
  std::map<const unsigned int, int> inputs;

  // look up table of position/normal/texcoord duplicate indices
  std::map<unsigned int, unsigned int> texDupMap;
  std::map<unsigned int, unsigned int> normalDupMap;
  std::map<unsigned int, unsigned int> positionDupMap;

  while (trianglesInputXml)
  {
    std::string semantic = trianglesInputXml->Attribute("semantic");
    std::string source = trianglesInputXml->Attribute("source");
    std::string offset = trianglesInputXml->Attribute("offset");
    if (semantic == "VERTEX")
    {
      unsigned int count = norms.size();
      this->LoadVertices(source, _transform, verts, norms,
          positionDupMap, normalDupMap);
      if (norms.size() > count)
        combinedVertNorms = true;
      inputs[VERTEX] = ignition::math::parseInt(offset);
      hasVertices = true;
    }
    else if (semantic == "NORMAL")
    {
      this->LoadNormals(source, _transform, norms, normalDupMap);
      combinedVertNorms = false;
      inputs[NORMAL] = ignition::math::parseInt(offset);
      hasNormals = true;
    }
    else if (semantic == "TEXCOORD" && !hasTexcoords)
    {
      // we currently only support one set of UVs
      this->LoadTexCoords(source, texcoords, texDupMap);
      inputs[TEXCOORD] = ignition::math::parseInt(offset);
      hasTexcoords = true;
    }
    else
    {
      inputs[otherSemantics++] = ignition::math::parseInt(offset);
      ignwarn << "Triangle input semantic: '" << semantic << "' is currently"
          << " not supported" << std::endl;
    }
    trianglesInputXml = trianglesInputXml->NextSiblingElement("input");
    offsetSize++;
  }

  tinyxml2::XMLElement *pXml = _trianglesXml->FirstChildElement("p");
  if (!pXml || !pXml->GetText())
  {
    int count = 1;
    if (_trianglesXml->Attribute("count"))
    {
      try
      {
        count = boost::lexical_cast<int>(_trianglesXml->Attribute("count"));
      }
      catch(...)
      {
        // Do nothing. Messages are printed out below.
      }
    }

    // It's possible that the triangle count is zero. In this case, we
    // should not output an error message
    if (count)
    {
      ignerr << "Collada file[" << this->dataPtr->filename
        << "] is invalid. Loading what we can...\n";
    }
    else
    {
      ignlog << "Triangle input has a count of zero. "
        << "This is likely not desired\n";
    }

    return;
  }
  std::string pStr = pXml->GetText();

  // Collada format allows normals and texcoords to have their own set of
  // indices for more efficient storage of data but opengl only supports one
  // index buffer. So we need to reorder normals/texcoord to match the vertex
  // index and duplicate any vertices that have the same index but different
  // normal/texcoord.

  // vertexIndexMap is a map of collada vertex index to Gazebo submesh vertex
  // indices, used for identifying vertices that can be shared.
  std::map<unsigned int, std::vector<GeometryIndices> > vertexIndexMap;

  unsigned int *values = new unsigned int[offsetSize];
  std::vector<std::string> strs;

  boost::split(strs, pStr, boost::is_any_of("   "));

  for (unsigned int j = 0; j < strs.size(); j += offsetSize)
  {
    for (unsigned int i = 0; i < offsetSize; ++i)
      values[i] = ignition::math::parseInt(strs[j+i]);

    unsigned int daeVertIndex = 0;
    bool addIndex = !hasVertices;

    // find a set of vertex/normal/texcoord that can be reused
    // only do this if the mesh has vertices
    if (hasVertices)
    {
      // Get the vertex position index value. If the position is a duplicate
      // then reset the index to the first instance of the duplicated position
      daeVertIndex = values[inputs[VERTEX]];
      if (positionDupMap.find(daeVertIndex) != positionDupMap.end())
        daeVertIndex = positionDupMap[daeVertIndex];

      // if the vertex index has not been previously added then just add it.
      if (vertexIndexMap.find(daeVertIndex) == vertexIndexMap.end())
      {
        addIndex = true;
      }
      else
      {
        // if the vertex index was previously added, check to see if it has the
        // same normal and texcoord index values
        bool toDuplicate = true;
        unsigned int reuseIndex = 0;
        std::vector<GeometryIndices> inputValues = vertexIndexMap[daeVertIndex];

        for (unsigned int i = 0; i < inputValues.size(); ++i)
        {
          GeometryIndices iv = inputValues[i];
          bool normEqual = false;
          bool texEqual = false;
          if (hasNormals)
          {
            // Get the vertex normal index value. If the normal is a duplicate
            // then reset the index to the first instance of the duplicated
            // position
            unsigned int remappedNormalIndex = values[inputs[NORMAL]];
            if (normalDupMap.find(remappedNormalIndex) != normalDupMap.end())
              remappedNormalIndex = normalDupMap[remappedNormalIndex];

            if (iv.normalIndex == remappedNormalIndex)
              normEqual = true;
          }
          if (hasTexcoords)
          {
            // Get the vertex texcoord index value. If the texcoord is a
            // duplicate then reset the index to the first instance of the
            // duplicated texcoord
            unsigned int remappedTexcoordIndex = values[inputs[TEXCOORD]];
            if (texDupMap.find(remappedTexcoordIndex) != texDupMap.end())
              remappedTexcoordIndex = texDupMap[remappedTexcoordIndex];

            if (iv.texcoordIndex == remappedTexcoordIndex)
              texEqual = true;
          }

          // if the vertex has matching normal and texcoord index values then
          // the vertex can be reused.
          if ((!hasNormals || normEqual) && (!hasTexcoords || texEqual))
          {
            // found a vertex that can be shared.
            toDuplicate = false;
            reuseIndex = iv.mappedIndex;
            subMesh->AddIndex(reuseIndex);
            break;
          }
        }
        addIndex = toDuplicate;
      }
    }

    // if the vertex index is new or can not be shared then add it
    if (addIndex)
    {
      GeometryIndices input;
      if (hasVertices)
      {
        subMesh->AddVertex(verts[daeVertIndex]);
        unsigned int newVertIndex = subMesh->VertexCount()-1;
        subMesh->AddIndex(newVertIndex);

        if (combinedVertNorms)
          subMesh->AddNormal(norms[daeVertIndex]);
        if (_mesh->HasSkeleton())
        {
          SkeletonPtr skel = _mesh->MeshSkeleton();
          for (unsigned int i = 0;
              i < skel->VertNodeWeightCount(values[daeVertIndex]); ++i)
          {
            std::pair<std::string, double> node_weight =
              skel->VertNodeWeight(values[daeVertIndex], i);
            SkeletonNode *node =
                _mesh->MeshSkeleton()->NodeByName(node_weight.first);
            subMesh->AddNodeAssignment(subMesh->VertexCount()-1,
                            node->Handle(), node_weight.second);
          }
        }
        input.vertexIndex = daeVertIndex;
        input.mappedIndex = newVertIndex;
      }
      if (hasNormals)
      {
        unsigned int inputRemappedNormalIndex = values[inputs[NORMAL]];
        if (normalDupMap.find(inputRemappedNormalIndex) != normalDupMap.end())
          inputRemappedNormalIndex = normalDupMap[inputRemappedNormalIndex];
        subMesh->AddNormal(norms[inputRemappedNormalIndex]);
        input.normalIndex = inputRemappedNormalIndex;
      }
      if (hasTexcoords)
      {
        unsigned int inputRemappedTexcoordIndex = values[inputs[TEXCOORD]];
        if (texDupMap.find(inputRemappedTexcoordIndex) != texDupMap.end())
          inputRemappedTexcoordIndex = texDupMap[inputRemappedTexcoordIndex];
        subMesh->AddTexCoord(texcoords[inputRemappedTexcoordIndex].X(),
            texcoords[inputRemappedTexcoordIndex].Y());
        input.texcoordIndex = inputRemappedTexcoordIndex;
      }

      // add the new ignition submesh vertex index to the map
      if (hasVertices)
      {
        std::vector<GeometryIndices> inputValues;
        inputValues.push_back(input);
        vertexIndexMap[daeVertIndex] = inputValues;
      }
    }
  }

  delete [] values;
  _mesh->AddSubMesh(subMesh);
}

/////////////////////////////////////////////////
void ColladaLoader::LoadLines(tinyxml2::XMLElement *_xml,
    const ignition::math::Matrix4d &_transform, Mesh *_mesh)
{
  SubMeshPtr subMesh(new SubMesh);
  subMesh->SetName(this->dataPtr->currentNodeName);
  subMesh->SetPrimitiveType(SubMesh::LINES);

  tinyxml2::XMLElement *inputXml = _xml->FirstChildElement("input");
  // std::string semantic = inputXml->Attribute("semantic");
  std::string source = inputXml->Attribute("source");

  std::vector<ignition::math::Vector3d> verts;
  std::vector<ignition::math::Vector3d> norms;
  this->LoadVertices(source, _transform, verts, norms);

  tinyxml2::XMLElement *pXml = _xml->FirstChildElement("p");
  std::string pStr = pXml->GetText();
  std::istringstream iss(pStr);

  do
  {
    int a, b;
    iss >> a >> b;

    if (!iss)
      break;
    subMesh->AddVertex(verts[a]);
    subMesh->AddIndex(subMesh->VertexCount() - 1);
    subMesh->AddVertex(verts[b]);
    subMesh->AddIndex(subMesh->VertexCount() - 1);
  } while (iss);

  _mesh->AddSubMesh(subMesh);
}

/////////////////////////////////////////////////
float ColladaLoader::LoadFloat(tinyxml2::XMLElement *_elem)
{
  float value = 0;

  if (_elem->FirstChildElement("float"))
  {
    value =
      ignition::math::parseFloat(_elem->FirstChildElement("float")->GetText());
  }

  return value;
}

/////////////////////////////////////////////////
void ColladaLoader::LoadTransparent(tinyxml2::XMLElement *_elem,
    MaterialPtr _mat)
{
  const char *opaqueCStr = _elem->Attribute("opaque");
  if (!opaqueCStr)
  {
    // ignerr << "No Opaque set\n";
    return;
  }

  // TODO: Handle transparent textures
  if (_elem->FirstChildElement("color"))
  {
    const char *colorCStr = _elem->FirstChildElement("color")->GetText();
    if (!colorCStr)
    {
      ignerr << "No color string\n";
      return;
    }

    std::string opaqueStr = opaqueCStr;
    std::string colorStr = colorCStr;
    Color color = boost::lexical_cast<Color>(colorStr);

    double srcFactor = 0;
    double dstFactor = 0;

    if (opaqueStr == "RGB_ZERO")
    {
      srcFactor = color.R() * _mat->Transparency();
      dstFactor = 1.0 - color.R() * _mat->Transparency();
    }
    else if (opaqueStr == "A_ONE")
    {
      srcFactor = 1.0 - color.A() * _mat->Transparency();
      dstFactor = color.A() * _mat->Transparency();
    }

    _mat->SetBlendFactors(srcFactor, dstFactor);
  }
}
