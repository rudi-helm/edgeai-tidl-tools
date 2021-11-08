/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <iostream>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <functional>
#include <vector>
#include <limits>
#include <stdexcept>
#include <dlr.h>
#include <libgen.h>
#include <utility>
#include <thread>
#include <chrono>
#include <mutex>
#include <pthread.h>

#include <memory.h>
#include <unistd.h>
#include <getopt.h>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc/imgproc_c.h"

#include "../cpp_config.h"
#include "../pre_process/pre_process.h"
#include "../post_process/post_process.h"

#define LOG(x) std::cerr

namespace dlr
{
  namespace main
  {
    std::vector<std::string> vecOfLabels;

    /**
 @struct  Settings
 @brief   This structure define the parameters of tfl cpp infernce params
*/
    struct Settings
    {
      bool verbose = false;
      bool accel = false;
      bool device_mem = false;
      bool input_floating = false;
      bool profiling = false;
      bool allow_fp16 = false;
      bool gl_backend = false;
      bool hexagon_delegate = false;
      int loop_count = 1;
      std::vector<float> input_mean;
      std::vector<float> input_std;
      std::string artifact_path = "";
      std::string model_path = "";
      std::string input_bmp_path = "";
      std::string labels_file_path = "";
      std::string input_node_name = "input.1";
      std::string input_layer_type = "uint8_t";
      int number_of_threads = 4;
      int number_of_results = 5;
      int max_profiling_buffer_entries = 1024;
      int number_of_warmup_runs = 2;
      tidl::config::Modeltype model_type;
      bool isFormatNCHW;
    };
    /**
          *  \brief display usage string for application
  * @returns void
  */
    void display_usage()
    {
      LOG(INFO)
          << "label_image\n"
          << "--accelerated, -a: [0|1], use Android NNAPI or not\n"
          << "--old_accelerated, -d: [0|1], use old Android NNAPI delegate or not\n"
          << "--artifact_path, -f: [0|1], Path for Delegate artifacts folder \n"
          << "--count, -c: loop interpreter->Invoke() for certain times\n"
          << "--gl_backend, -g: use GL GPU Delegate on Android\n"
          << "--input_mean, -b: input mean\n"
          << "--input_std, -s: input standard deviation\n"
          << "--image, -i: image_name.bmp\n"
          << "--labels, -l: labels for the model\n"
          << "--tflite_model, -m: model_name.tflite\n"
          << "--profiling, -p: [0|1], profiling or not\n"
          << "--num_results, -r: number of results to show\n"
          << "--threads, -t: number of threads\n"
          << "--verbose, -v: [0|1] print more information\n"
          << "--warmup_runs, -w: number of warmup runs\n"
          << "\n";
    }
    /**
  *  \brief  Case Sensitive Implementation of endsWith for a string
  * It checks if the string 'mainStr' ends with given string 'toMatch'
  * @returns bool
  */
    bool endsWith(const std::string &mainStr, const std::string &toMatch)
    {
      if (mainStr.size() >= toMatch.size() &&
          mainStr.compare(mainStr.size() - toMatch.size(), toMatch.size(), toMatch) == 0)
        return true;
      else
        return false;
    }
  }
}