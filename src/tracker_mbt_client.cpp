#include <cstdlib>
#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include <ros/ros.h>
#include <ros/param.h>
#include <dynamic_reconfigure/server.h>
#include <image_transport/image_transport.h>
#include <visp_tracker/Init.h>
#include <visp_tracker/MovingEdgeConfig.h>

#include <visp/vpMe.h>

#define protected public
#include <visp/vpMbEdgeTracker.h>
#undef protected

#include <visp/vpDisplayX.h>

#include "conversion.hh"
#include "callbacks.hh"
#include "file.hh"
#include "names.hh"

//TODO: synchronize messages for display!

typedef vpImage<unsigned char> image_t;

int main(int argc, char **argv)
{
  std::string camera_prefix;
  std::string tracker_prefix;
  std::string model_path;
  std::string model_name;
  vpMe moving_edge;

  bool track = false;

  image_t I;

  vpMbEdgeTracker tracker;
  tracker.resetTracker();

  // Initialization.
  ros::init(argc, argv, "tracker_mbt_client");

  ros::NodeHandle n;
  image_transport::ImageTransport it(n);

  // Parameters.
  ros::param::param<std::string>("~camera_prefix", camera_prefix, "");
  ros::param::param<std::string>("~tracker_prefix",
				 tracker_prefix,
				 visp_tracker::default_tracker_prefix);

  ros::param::param<std::string>("~model_path", model_path,
				 visp_tracker::default_model_path);
  ros::param::param<std::string>("~model_name", model_name, "");

  // Compute topic and services names.
  const std::string rectified_image_topic =
    ros::names::clean(camera_prefix + "/image_rect");
  const std::string camera_info_topic =
    ros::names::clean(camera_prefix + "/camera_info");

  const std::string init_service =
    ros::names::clean(tracker_prefix + "/" + visp_tracker::init_service);


  visp_tracker::MovingEdgeConfig defaultMovingEdge = 
    visp_tracker::MovingEdgeConfig::__getDefault__();
  ros::param::param("~vpme_mask_size", moving_edge.mask_size,
		    defaultMovingEdge.mask_size);
  ros::param::param("~vpme_n_mask", moving_edge.n_mask,
		    defaultMovingEdge.n_mask);
  ros::param::param("~vpme_range", moving_edge.range,
		    defaultMovingEdge.range);
  ros::param::param("~vpme_threshold", moving_edge.threshold,
		    defaultMovingEdge.threshold);
  ros::param::param("~vpme_mu1", moving_edge.mu1,
		    defaultMovingEdge.mu1);
  ros::param::param("~vpme_mu2", moving_edge.mu2,
		    defaultMovingEdge.mu2);
  ros::param::param("~vpme_sample_step", moving_edge.sample_step,
		    defaultMovingEdge.sample_step);
  ros::param::param("~vpme_ntotal_sample", moving_edge.ntotal_sample,
		    defaultMovingEdge.ntotal_sample);

  ros::param::param("~track", track, false);

  // Dynamic reconfigure.
  dynamic_reconfigure::Server<visp_tracker::MovingEdgeConfig> reconfigureSrv;
  dynamic_reconfigure::Server<visp_tracker::MovingEdgeConfig>::CallbackType f;
  f = boost::bind(&reconfigureCallback, boost::ref(tracker),
		  boost::ref(moving_edge), _1, _2);
  reconfigureSrv.setCallback(f);

  // Camera subscriber.
  std_msgs::Header header;
  sensor_msgs::CameraInfoConstPtr info;
  image_transport::CameraSubscriber sub =
    it.subscribeCamera(rectified_image_topic, 100,
		       bindImageCallback(I, header, info));

  // Model loading.
  boost::filesystem::path vrml_path =
    getModelFileFromModelName(model_name, model_path);
  boost::filesystem::path init_path =
    getInitFileFromModelName(model_name, model_path);

  ROS_INFO_STREAM("VRML file: " << vrml_path);
  ROS_INFO_STREAM("Init file: " << init_path);

  // Check that required files exist.
  if (!boost::filesystem::is_regular_file(vrml_path))
    {
      ROS_ERROR_STREAM("VRML model " << vrml_path << " is not a regular file.");
      return 1;
    }
  if (!boost::filesystem::is_regular_file(init_path))
    {
      ROS_ERROR_STREAM("Init file " << vrml_path << " is not a regular file.");
      return 1;
    }

  // Load the 3d model.
  try
    {
      ROS_DEBUG_STREAM("Trying to load the model "
		<< vrml_path.external_file_string());
      tracker.loadModel(vrml_path.external_file_string().c_str());
      ROS_INFO("Model has been successfully loaded.");

      ROS_INFO_STREAM("Nb hidden faces: "
		      << tracker.faces.getPolygon().nbElements());
      ROS_INFO_STREAM("Nb line: " << tracker.Lline.nbElements());
      ROS_INFO_STREAM("nline: " << tracker.nline);
      ROS_INFO_STREAM("Visible faces: " << tracker.nbvisiblepolygone);
    }
  catch(...)
    {
      ROS_ERROR_STREAM("Failed to load the model " << vrml_path);
      return 1;
    }

  // Wait for the image to be initialized.
  ros::Rate loop_rate(10);
  while (!I.getWidth() || !I.getHeight())
    {
      ros::spinOnce();
      loop_rate.sleep();
    }

  // Tracker initialization.

  // - Camera
  vpCameraParameters cam;
  try
    {
      initializeVpCameraFromCameraInfo(cam, info);
    }
  catch(std::exception& e)
    {
      ROS_ERROR_STREAM("failed to initialize camera: " << e.what());
      return 1;
    }
  tracker.setCameraParameters(cam);
  tracker.setDisplayMovingEdges(true);

  // - Moving edges.
  moving_edge.initMask();
  tracker.setMovingEdge(moving_edge);

  // Display camera parameters and moving edges settings.
  ROS_INFO_STREAM(cam);
  moving_edge.print();

  vpDisplayX d(I, I.getWidth(), I.getHeight(),
	       "ViSP MBT tracker initialization");

  ros::Rate loop_rate_tracking(200);
  bool ok = false;
  vpHomogeneousMatrix cMo;
  vpImagePoint point (10, 10);
  while (!ok)
    {
      try
	{
	  // Initialize.
	  vpDisplay::display(I);
	  vpDisplay::flush(I);
	  std::string init_file
	    (init_path.replace_extension().external_file_string());
	  tracker.initClick(I, init_file.c_str());
	  tracker.getPose(cMo);

	  ROS_INFO_STREAM("initial pose [tx,ty,tz,tux,tuy,tuz]:\n"
			  << vpPoseVector(cMo));

	  // Track once to make sure initialization is correct.
	  do
	    {
	      vpDisplay::display(I);
	      tracker.track(I);
	      tracker.display(I, cMo, cam, vpColor::red, 2);
	      vpDisplay::displayCharString
		(I, point, "first tracking", vpColor::red);
	      vpDisplay::flush(I);
	      tracker.getPose(cMo);

	      ros::spinOnce();
	      loop_rate_tracking.sleep();
	    }
	  while (track);
	  vpDisplay::getClick(I);
	  ok = true;
	}
      catch(const std::string& str)
	{
	  ROS_ERROR_STREAM("failed to initialize: " << str << ", retrying...");
	}
      catch(...)
	{
	  ROS_ERROR("failed to initialize, retrying...");
	}
    }

  ROS_INFO_STREAM("Initialization done, sending initial cMo:\n" << cMo);

  ros::ServiceClient client = n.serviceClient<visp_tracker::Init>(init_service);
  visp_tracker::Init srv;

  srv.request.model_path.data = model_path;
  srv.request.model_name.data = model_name;
  vpHomogeneousMatrixToTransform(srv.request.initial_cMo, cMo);

  convertVpMeToInitRequest(moving_edge, tracker, srv);

  if (client.call(srv))
  {
    if (srv.response.initialization_succeed)
      ROS_INFO("Tracker initialized with success.");
    else
      {
	ROS_ERROR("Failed to initialize tracker.");
	return 2;
      }
  }
  else
  {
    ROS_ERROR("Failed to call service init");
    return 1;
  }

  return 0;
}
