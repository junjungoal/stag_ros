/**
MIT License

Copyright (c) 2020 Michail Kalaitzakis (Unmanned Systems and Robotics Lab,
University of South Carolina, USA)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Project includes
#include "stag_ros/stag_nodelet.h"
#include "stag_ros/instrument.hpp"
#include "stag_ros/StagMarker.h"
#include "stag_ros/StagMarkers.h"

// Stag marker handle
#include "Marker.h"

// ROS includes
#include "tf/tf.h"
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>

#include <iostream>
#include <swri_nodelet/class_list_macros.h>

SWRI_NODELET_EXPORT_CLASS(stag_ros, StagNodelet)

namespace stag_ros {

void StagNodelet::onInit() {
  ros::NodeHandle nh = getNodeHandle();
  image_transport::ImageTransport imageT(nh);

  // Load Parameters
  loadParameters();

  // Set Subscribers
  imageSub = imageT.subscribe(
      imageTopic, 1, &StagNodelet::imageCallback, this,
      image_transport::TransportHints(isCompressed ? "compressed" : "raw"));

  cameraInfoSub =
      nh.subscribe(cameraInfoTopic, 1, &StagNodelet::cameraInfoCallback, this);

  // Set Publishers
  markersPub = nh.advertise<stag_ros::StagMarkers>("/stag/markers", 1);
  if (debugI) imageDebugPub = imageT.advertise("/stag/image_markers", 1);

  // Initialize Stag
  stag = new Stag(stagLib, errorC, false);

  // Initialize camera info
  gotCamInfo = false;
  cameraMatrix = cv::Mat::zeros(3, 3, CV_64F);
  distortionMat = cv::Mat::zeros(1, 5, CV_64F);
  rectificationMat = cv::Mat::zeros(3, 3, CV_64F);
  projectionMat = cv::Mat::zeros(3, 4, CV_64F);

  // Initialize tag corners
  double halfTagSize = 0.5 * tagSize;
  tagCorners.push_back(cv::Point3f(0.0, 0.0, 0.0));
  tagCorners.push_back(cv::Point3f(-halfTagSize, halfTagSize, 0.0));
  tagCorners.push_back(cv::Point3f(halfTagSize, halfTagSize, 0.0));
  tagCorners.push_back(cv::Point3f(halfTagSize, -halfTagSize, 0.0));
  tagCorners.push_back(cv::Point3f(-halfTagSize, -halfTagSize, 0.0));
}  // namespace stag_ros

StagNodelet::~StagNodelet() { delete stag; }

void StagNodelet::loadParameters() {
  // Create private nodeHandle to load parameters
  ros::NodeHandle nh_lcl = getNodeHandle();

  nh_lcl.param("libraryHD", stagLib, 15);
  nh_lcl.param("errorCorrection", errorC, 7);
  nh_lcl.param("tagSize", tagSize, 14.6);
  nh_lcl.param("raw_image_topic", imageTopic, std::string("usb_cam/image_raw"));
  nh_lcl.param("camera_info_topic", cameraInfoTopic,
               std::string("usb_cam/camera_info"));
  nh_lcl.param("is_compressed", isCompressed, false);
  nh_lcl.param("debug_images", debugI, false);
  nh_lcl.param("tag_tf_prefix", tag_tf_prefix, std::string("stag_"));
}

void StagNodelet::imageCallback(const sensor_msgs::ImageConstPtr& msg) {
  INSTRUMENT;
  static tf::TransformBroadcaster br;
  if (gotCamInfo) {
    // Read image from msg and convert it to grayscale, checks provided for rgb8 and bgr8, default to mono8
    cv::Mat gray;
    if (msg->encoding.compare("bgr8") == 0) {
      cv::Mat src = cv_bridge::toCvShare(msg, msg->encoding)->image;
      cv::cvtColor(src, gray, CV_BGR2GRAY);
    } else if (msg->encoding.compare("rgb8") == 0) {
      cv::Mat src = cv_bridge::toCvShare(msg, msg->encoding)->image;
      cv::cvtColor(src, gray, CV_RGB2GRAY);
    } else if(msg->encoding.compare("mono8")==0) {
      gray = cv_bridge::toCvShare(msg, msg->encoding)->image;
    } else {
      NODELET_FATAL("Wrong image encoding: %s. You must add support at line %i.",msg->encoding.c_str(),__LINE__);
      ros::shutdown();
    }

    // Process the image to find the markers
    stag->detectMarkers(gray);
    std::vector<Marker> markers = stag->getMarkerList();

    // Publish debug image
    if (debugI) {
      cv_bridge::CvImage rosMat;
      rosMat.header = msg->header;
      rosMat.encoding = "bgr8";
      rosMat.image = stag->drawMarkers();

      sensor_msgs::Image rosImage;
      rosMat.toImageMsg(rosImage);

      imageDebugPub.publish(rosImage);
    }

    // For each marker in the list
    if (markers.size() > 0) {
      // Create markers msg
      stag_ros::StagMarkers markersMsg;

      for (int i = 0; i < markers.size(); i++) {
        // Create marker msg
        stag_ros::StagMarker markerMsg;
        markerMsg.id = markers[i].id;

        // Solve PnP to find tag pose
        std::vector<cv::Point2f> imageCorners;
        imageCorners.push_back(markers[i].center);
        imageCorners.push_back(markers[i].corners[0]);
        imageCorners.push_back(markers[i].corners[1]);
        imageCorners.push_back(markers[i].corners[2]);
        imageCorners.push_back(markers[i].corners[3]);

        cv::Mat rVec, rMat, tVec;

        cv::solvePnP(tagCorners, imageCorners, cameraMatrix, distortionMat,
                     rVec, tVec);
        cv::Rodrigues(rVec, rMat);

        tf::Matrix3x3 rotMat(rMat.at<double>(0, 0), rMat.at<double>(0, 1),
                             rMat.at<double>(0, 2), rMat.at<double>(1, 0),
                             rMat.at<double>(1, 1), rMat.at<double>(1, 2),
                             rMat.at<double>(2, 0), rMat.at<double>(2, 1),
                             rMat.at<double>(2, 2));
        tf::Quaternion rotQ;
        rotMat.getRotation(rotQ);
        markerMsg.pose.position.x = tVec.at<double>(0);
        markerMsg.pose.position.y = tVec.at<double>(1);
        markerMsg.pose.position.z = tVec.at<double>(2);
        tf::quaternionTFToMsg(rotQ, markerMsg.pose.orientation);
        markersMsg.markers.push_back(markerMsg);

        tf::Vector3 tfVec(tVec.at<double>(0),tVec.at<double>(1),tVec.at<double>(2));
        br.sendTransform(tf::StampedTransform(tf::Transform(rotQ,tfVec),msg->header.stamp,msg->header.frame_id,tag_tf_prefix+std::to_string(markers[i].id)));

      }
      markersPub.publish(markersMsg);
    } else
      ROS_WARN("No markers detected");
  }
}

void StagNodelet::cameraInfoCallback(
    const sensor_msgs::CameraInfoConstPtr& msg) {
  if (!gotCamInfo) {
    // Get camera Matrix
    cameraMatrix.at<double>(0, 0) = msg->K[0];
    cameraMatrix.at<double>(0, 1) = msg->K[1];
    cameraMatrix.at<double>(0, 2) = msg->K[2];
    cameraMatrix.at<double>(1, 0) = msg->K[3];
    cameraMatrix.at<double>(1, 1) = msg->K[4];
    cameraMatrix.at<double>(1, 2) = msg->K[5];
    cameraMatrix.at<double>(2, 0) = msg->K[6];
    cameraMatrix.at<double>(2, 1) = msg->K[7];
    cameraMatrix.at<double>(2, 2) = msg->K[8];

    // Get distortion Matrix
    distortionMat.at<double>(0, 0) = msg->D[0];
    distortionMat.at<double>(0, 1) = msg->D[1];
    distortionMat.at<double>(0, 2) = msg->D[2];
    distortionMat.at<double>(0, 3) = msg->D[3];
    distortionMat.at<double>(0, 4) = msg->D[4];
    // Get rectification Matrix
    rectificationMat.at<double>(0, 0) = msg->R[0];
    rectificationMat.at<double>(0, 1) = msg->R[1];
    rectificationMat.at<double>(0, 2) = msg->R[2];
    rectificationMat.at<double>(1, 0) = msg->R[3];
    rectificationMat.at<double>(1, 1) = msg->R[4];
    rectificationMat.at<double>(1, 2) = msg->R[5];
    rectificationMat.at<double>(2, 0) = msg->R[6];
    rectificationMat.at<double>(2, 1) = msg->R[7];
    rectificationMat.at<double>(2, 2) = msg->R[8];
    // Get projection Matrix
    projectionMat.at<double>(0, 0) = msg->P[0];
    projectionMat.at<double>(0, 1) = msg->P[1];
    projectionMat.at<double>(0, 2) = msg->P[2];
    projectionMat.at<double>(1, 0) = msg->P[3];
    projectionMat.at<double>(1, 1) = msg->P[4];
    projectionMat.at<double>(1, 2) = msg->P[5];
    projectionMat.at<double>(2, 0) = msg->P[6];
    projectionMat.at<double>(2, 1) = msg->P[7];
    projectionMat.at<double>(2, 2) = msg->P[8];
    projectionMat.at<double>(2, 0) = msg->P[9];
    projectionMat.at<double>(2, 1) = msg->P[10];
    projectionMat.at<double>(2, 2) = msg->P[11];

    gotCamInfo = true;
  }
}
}  // namespace stag_ros