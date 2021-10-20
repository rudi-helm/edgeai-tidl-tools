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

#include "od.h"

#include <fcntl.h>     // NOLINT(build/include_order)
#include <getopt.h>    // NOLINT(build/include_order)
#include <sys/time.h>  // NOLINT(build/include_order)
#include <sys/types.h> // NOLINT(build/include_order)
#include <sys/uio.h>   // NOLINT(build/include_order)
#include <unistd.h>    // NOLINT(build/include_order)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/optional_debug_tools.h"
#include "tensorflow/lite/profiling/profiler.h"
#include "tensorflow/lite/string_util.h"
#include "dlfcn.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc/imgproc_c.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <functional>
#include <queue>

#include "tensorflow/lite/tools/evaluation/utils.h"

#include "itidl_rt.h"

#define LOG(x) std::cerr

namespace tflite
{
  namespace od_image
  {

    void *in_ptrs[16] = {NULL};
    void *out_ptrs[16] = {NULL};

    double get_us(struct timeval t) { return (t.tv_sec * 1000000 + t.tv_usec); }

    template <class T>
    int preprocImage(const std::string &input_bmp_name, T *out, int wanted_height, int wanted_width, int wanted_channels, float mean, float scale)
    {
      int i;
      uint8_t *pSrc;
      cv::Mat image = cv::imread(input_bmp_name, cv::IMREAD_COLOR);
      cv::cvtColor(image, image, cv::COLOR_BGR2RGB);
      cv::resize(image, image, cv::Size(wanted_width, wanted_height), 0, 0, cv::INTER_AREA);
      if (image.channels() != wanted_channels)
      {
        printf("Warning : Number of channels wanted differs from number of channels in the actual image \n");
        return (-1);
      }
      pSrc = (uint8_t *)image.data;
      for (i = 0; i < wanted_height * wanted_width * wanted_channels; i++)
        out[i] = ((T)pSrc[i] - mean) / scale;
      return 0;
    }

    /**
 * Use OpenCV to do in-place update of a buffer with post processing content like
 * drawing bounding box around a detected object in the frame. Typically used for
 * object classification models.
 * Although OpenCV expects BGR data, this function adjusts the color values so that
 * the post processing can be done on a RGB buffer without extra performance impact.
 *
 * @param img Original RGB data 
 * @param box bounding box co-ordinates.
 *
 * @returns original frame with some in-place post processing done
 */
    cv::Mat overlayBoundingBox(cv::Mat img, int num_of_detection, const float *cordinates)
    {
      cv::Scalar box_color = (20, 220, 20);
      for (int i = 0; i < num_of_detection; i++)
      {
        /* hard coded colour of box */
        cv::Scalar boxColor = cv::Scalar(20,20,20);
        int boxThickness = 3;
        float ymin = cordinates[i * 4 + 0];
        float xmin = cordinates[i * 4 + 1];
        float ymax = cordinates[i * 4 + 2];
        float xmax = cordinates[i * 4 + 3];
        cv::Point topleft = cv::Point(xmin * img.cols, ymax * img.rows);
        cv::Point bottomright = cv::Point(xmax * img.cols, ymin * img.rows);
        cv::rectangle(img, topleft, bottomright, box_color, boxThickness, cv::LINE_8);
      }
      /*fix the location and name of the saving image file */
      bool check = cv::imwrite("./name.jpg", img);
      if (check == false)
      {
        std::cout << "Saving the image, FAILED" << std::endl;
      }
      return img;
    }

    void RunInference(Settings *s)
    {
      if (!s->model_name.c_str())
      {
        LOG(ERROR) << "no model file name\n";
        exit(-1);
      }

      std::unique_ptr<tflite::FlatBufferModel> model;
      std::unique_ptr<tflite::Interpreter> interpreter;
      model = tflite::FlatBufferModel::BuildFromFile(s->model_name.c_str());
      if (!model)
      {
        LOG(FATAL) << "\nFailed to mmap model " << s->model_name << "\n";
        exit(-1);
      }
      s->model = model.get();
      LOG(INFO) << "Loaded model " << s->model_name << "\n";
      model->error_reporter();
      LOG(INFO) << "resolved reporter\n";

      tflite::ops::builtin::BuiltinOpResolver resolver;
      tflite::InterpreterBuilder(*model, resolver)(&interpreter);
      if (!interpreter)
      {
        LOG(FATAL) << "Failed to construct interpreter\n";
        exit(-1);
      }

      if (s->verbose)
      {
        LOG(INFO) << "tensors size: " << interpreter->tensors_size() << "\n";
        LOG(INFO) << "nodes size: " << interpreter->nodes_size() << "\n";
        LOG(INFO) << "inputs: " << interpreter->inputs().size() << "\n";
        LOG(INFO) << "input(0) name: " << interpreter->GetInputName(0) << "\n";

        int t_size = interpreter->tensors_size();
        for (int i = 0; i < t_size; i++)
        {
          if (interpreter->tensor(i)->name)
            LOG(INFO) << i << ": " << interpreter->tensor(i)->name << ", "
                      << interpreter->tensor(i)->bytes << ", "
                      << interpreter->tensor(i)->type << ", "
                      << interpreter->tensor(i)->params.scale << ", "
                      << interpreter->tensor(i)->params.zero_point << "\n";
        }
      }

      if (s->number_of_threads != -1)
      {
        interpreter->SetNumThreads(s->number_of_threads);
      }

      int input = interpreter->inputs()[0];
      if (s->verbose)
        LOG(INFO) << "input: " << input << "\n";

      const std::vector<int> inputs = interpreter->inputs();
      const std::vector<int> outputs = interpreter->outputs();

      if (s->verbose)
      {
        LOG(INFO) << "number of inputs: " << inputs.size() << "\n";
        LOG(INFO) << "number of outputs: " << outputs.size() << "\n";
      }

      if (s->accel == 1)
      {
        char artifact_path[512];
        /* This part creates the dlg_ptr */
        typedef TfLiteDelegate *(*tflite_plugin_create_delegate)(char **, char **, size_t, void (*report_error)(const char *));
        tflite_plugin_create_delegate tflite_plugin_dlg_create;
        char *keys[] = {"artifacts_folder", "num_tidl_subgraphs", "debug_level"};
        char *values[] = {(char *)s->artifact_path.c_str(), "16", "0"};
        void *lib = dlopen("libtidl_tfl_delegate.so", RTLD_NOW);
        assert(lib);
        tflite_plugin_dlg_create = (tflite_plugin_create_delegate)dlsym(lib, "tflite_plugin_create_delegate");
        TfLiteDelegate *dlg_ptr = tflite_plugin_dlg_create(keys, values, 3, NULL);
        interpreter->ModifyGraphWithDelegate(dlg_ptr);
        printf("ModifyGraphWithDelegate - Done \n");
      }

      if (interpreter->AllocateTensors() != kTfLiteOk)
      {
        LOG(FATAL) << "Failed to allocate tensors!";
      }

      if (s->device_mem)
      {
        for (uint32_t i = 0; i < inputs.size(); i++)
        {
          const TfLiteTensor *tensor = interpreter->input_tensor(i);
          in_ptrs[i] = TIDLRT_allocSharedMem(tflite::kDefaultTensorAlignment, tensor->bytes);
          if (in_ptrs[i] == NULL)
          {
            LOG(FATAL) << "Could not allocate Memory for input: " << tensor->name << "\n";
          }
          interpreter->SetCustomAllocationForTensor(inputs[i], {in_ptrs[i], tensor->bytes});
        }
        for (uint32_t i = 0; i < outputs.size(); i++)
        {
          const TfLiteTensor *tensor = interpreter->output_tensor(i);
          out_ptrs[i] = TIDLRT_allocSharedMem(tflite::kDefaultTensorAlignment, tensor->bytes);
          if (out_ptrs[i] == NULL)
          {
            LOG(FATAL) << "Could not allocate Memory for ouput: " << tensor->name << "\n";
          }
          interpreter->SetCustomAllocationForTensor(outputs[i], {out_ptrs[i], tensor->bytes});
        }
      }

      if (s->verbose)
        PrintInterpreterState(interpreter.get());

      /* get input dimension from the input tensor metadata
      assuming one input only */
      TfLiteIntArray *dims = interpreter->tensor(input)->dims;
      int wanted_height = dims->data[1];
      int wanted_width = dims->data[2];
      int wanted_channels = dims->data[3];

      int status = -1;
      switch (interpreter->tensor(input)->type)
      {
      case kTfLiteFloat32:
        status = tflite::od_image::preprocImage<float>(s->input_bmp_name, interpreter->typed_tensor<float>(input), wanted_height, wanted_width, wanted_channels, s->input_mean, s->input_std);
        break;
      case kTfLiteUInt8:
        status = tflite::od_image::preprocImage<uint8_t>(s->input_bmp_name, interpreter->typed_tensor<uint8_t>(input), wanted_height, wanted_width, wanted_channels, s->input_mean, s->input_std);
        break;
      default:
        LOG(FATAL) << "cannot handle input type " << interpreter->tensor(input)->type << " yet";
        exit(-1);
      }
      if (status == -1)
      {
        LOG(FATAL) << "Could not read input for  " << interpreter->tensor(input)->name;
        exit(-1);
      }

      printf("interpreter->Invoke - Started \n");

      if (s->loop_count > 1)
        for (int i = 0; i < s->number_of_warmup_runs; i++)
        {
          if (interpreter->Invoke() != kTfLiteOk)
          {
            LOG(FATAL) << "Failed to invoke tflite!\n";
          }
        }

      struct timeval start_time, stop_time;
      gettimeofday(&start_time, nullptr);
      for (int i = 0; i < s->loop_count; i++)
      {
        if (interpreter->Invoke() != kTfLiteOk)
        {
          LOG(FATAL) << "Failed to invoke tflite!\n";
        }
      }
      gettimeofday(&stop_time, nullptr);
      printf("interpreter->Invoke - Done \n");

      LOG(INFO) << "invoked \n";
      LOG(INFO) << "average time: "
                << (get_us(stop_time) - get_us(start_time)) / (s->loop_count * 1000)
                << " ms \n";
      const float threshold = 0.001f;
      
      const std::vector<int> outputTensor = interpreter->outputs();
      const float *detectection_location = interpreter->tensor(outputTensor[0])->data.f;
      const float *detectection_classes = interpreter->tensor(outputTensor[1])->data.f;
      const float *detectection_scores = interpreter->tensor(outputTensor[2])->data.f;
      const int num_detections = (int)* interpreter->tensor(outputTensor[3])->data.f;
      LOG(INFO) << "results " << num_detections << "\n";
      cv::Mat img = cv::imread(s->input_bmp_name, cv::IMREAD_COLOR);
      tflite::od_image::overlayBoundingBox(img, num_detections, detectection_location);
      for (int i = 0; i < num_detections; i++)
      {
        LOG(INFO) << "class " << detectection_classes[i] << "\n";
        LOG(INFO) << "cordinates " << detectection_location[i * 4] << detectection_location[i * 4 + 1] << detectection_location[i * 4 + 2] << detectection_location[i * 4 + 3] << "\n";
        LOG(INFO) << "score " << detectection_scores[i] << "\n";
      }

      if (s->device_mem)
      {
        for (uint32_t i = 0; i < inputs.size(); i++)
        {
          if (in_ptrs[i])
          {
            TIDLRT_freeSharedMem(in_ptrs[i]);
          }
        }
        for (uint32_t i = 0; i < outputs.size(); i++)
        {
          if (out_ptrs[i])
          {
            TIDLRT_freeSharedMem(out_ptrs[i]);
          }
        }
      }
    }

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

    int TFLite_Main(int argc, char **argv)
    {
      Settings s;
      int c;
      while (1)
      {
        static struct option long_options[] = {
            {"accelerated", required_argument, nullptr, 'a'},
            {"device_mem", required_argument, nullptr, 'd'},
            {"artifact_path", required_argument, nullptr, 'f'},
            {"count", required_argument, nullptr, 'c'},
            {"verbose", required_argument, nullptr, 'v'},
            {"image", required_argument, nullptr, 'i'},
            {"tflite_model", required_argument, nullptr, 'm'},
            {"profiling", required_argument, nullptr, 'p'},
            {"threads", required_argument, nullptr, 't'},
            {"input_mean", required_argument, nullptr, 'b'},
            {"input_std", required_argument, nullptr, 's'},
            {"num_results", required_argument, nullptr, 'r'},
            {"max_profiling_buffer_entries", required_argument, nullptr, 'e'},
            {"warmup_runs", required_argument, nullptr, 'w'},
            {"gl_backend", required_argument, nullptr, 'g'},
            {nullptr, 0, nullptr, 0}};

        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long(argc, argv,
                        "a:b:c:d:e:f:g:i:m:p:r:s:t:v:w:", long_options,
                        &option_index);

        /* Detect the end of the options. */
        if (c == -1)
          break;

        switch (c)
        {
        case 'a':
          s.accel = strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'b':
          s.input_mean = strtod(optarg, nullptr);
          break;
        case 'c':
          s.loop_count =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'd':
          s.device_mem =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'e':
          s.max_profiling_buffer_entries =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'f':
          s.artifact_path = optarg;
          break;
        case 'g':
          s.gl_backend =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'i':
          s.input_bmp_name = optarg;
          break;
        case 'm':
          s.model_name = optarg;
          break;
        case 'p':
          s.profiling =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'r':
          s.number_of_results =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 's':
          s.input_std = strtod(optarg, nullptr);
          break;
        case 't':
          s.number_of_threads = strtol( // NOLINT(runtime/deprecated_fn)
              optarg, nullptr, 10);
          break;
        case 'v':
          s.verbose =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'w':
          s.number_of_warmup_runs =
              strtol(optarg, nullptr, 10); // NOLINT(runtime/deprecated_fn)
          break;
        case 'h':
        case '?':
          /* getopt_long already printed an error message. */
          display_usage();
          exit(-1);
        default:
          exit(-1);
        }
      }
      RunInference(&s);
      return 0;
    }

  } // namespace od_image
} // namespace tflite

int main(int argc, char **argv)
{
  return tflite::od_image::TFLite_Main(argc, argv);
}