/* Copyright 2023 The MediaPipe Authors. All Rights Reserved.

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

#include <memory>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "mediapipe/calculators/core/split_vector_calculator.pb.h"
#include "mediapipe/calculators/image/image_cropping_calculator.pb.h"
#include "mediapipe/calculators/image/warp_affine_calculator.pb.h"
#include "mediapipe/calculators/tensor/image_to_tensor_calculator.pb.h"
#include "mediapipe/calculators/util/landmarks_to_detection_calculator.pb.h"
#include "mediapipe/framework/api2/builder.h"
#include "mediapipe/framework/api2/port.h"
#include "mediapipe/framework/formats/image.h"
#include "mediapipe/framework/formats/landmark.pb.h"
#include "mediapipe/framework/formats/rect.pb.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/framework/port/status_macros.h"
#include "mediapipe/gpu/gpu_origin.pb.h"
#include "mediapipe/tasks/cc/common.h"
#include "mediapipe/tasks/cc/components/processors/image_preprocessing_graph.h"
#include "mediapipe/tasks/cc/core/model_resources_cache.h"
#include "mediapipe/tasks/cc/core/model_task_graph.h"
#include "mediapipe/tasks/cc/core/proto/external_file.pb.h"
#include "mediapipe/tasks/cc/metadata/utils/zip_utils.h"
#include "mediapipe/tasks/cc/vision/face_detector/proto/face_detector_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/face_landmarker/proto/face_landmarker_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/face_landmarker/proto/face_landmarks_detector_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/face_stylizer/calculators/tensors_to_image_calculator.pb.h"
#include "mediapipe/tasks/cc/vision/face_stylizer/proto/face_stylizer_graph_options.pb.h"

namespace mediapipe {
namespace tasks {
namespace vision {
namespace face_stylizer {

namespace {

using ::mediapipe::api2::Input;
using ::mediapipe::api2::Output;
using ::mediapipe::api2::builder::Graph;
using ::mediapipe::api2::builder::Source;
using ::mediapipe::tasks::TensorsToImageCalculatorOptions;
using ::mediapipe::tasks::core::ModelAssetBundleResources;
using ::mediapipe::tasks::core::ModelResources;
using ::mediapipe::tasks::core::proto::ExternalFile;
using ::mediapipe::tasks::metadata::SetExternalFile;
using ::mediapipe::tasks::vision::face_landmarker::proto::
    FaceLandmarkerGraphOptions;
using ::mediapipe::tasks::vision::face_stylizer::proto::
    FaceStylizerGraphOptions;

constexpr char kDetectionTag[] = "DETECTION";
constexpr char kFaceDetectorTFLiteName[] = "face_detector.tflite";
constexpr char kFaceLandmarksDetectorTFLiteName[] =
    "face_landmarks_detector.tflite";
constexpr char kFaceStylizerTFLiteName[] = "face_stylizer.tflite";
constexpr char kImageTag[] = "IMAGE";
constexpr char kImageCpuTag[] = "IMAGE_CPU";
constexpr char kImageGpuTag[] = "IMAGE_GPU";
constexpr char kImageSizeTag[] = "IMAGE_SIZE";
constexpr char kMatrixTag[] = "MATRIX";
constexpr char kNormLandmarksTag[] = "NORM_LANDMARKS";
constexpr char kNormRectTag[] = "NORM_RECT";
constexpr char kOutputSizeTag[] = "OUTPUT_SIZE";
constexpr char kSizeTag[] = "SIZE";
constexpr char kStylizedImageTag[] = "STYLIZED_IMAGE";
constexpr char kTensorsTag[] = "TENSORS";

// Struct holding the different output streams produced by the face stylizer
// graph.
struct FaceStylizerOutputStreams {
  Source<Image> stylized_image;
  Source<Image> original_image;
};

// Sets the base options in the sub tasks.
absl::Status SetSubTaskBaseOptions(const ModelAssetBundleResources& resources,
                                   FaceStylizerGraphOptions* options,
                                   ExternalFile* face_stylizer_external_file,
                                   bool is_copy) {
  auto* face_detector_graph_options =
      options->mutable_face_landmarker_graph_options()
          ->mutable_face_detector_graph_options();
  if (!face_detector_graph_options->base_options().has_model_asset()) {
    ASSIGN_OR_RETURN(const auto face_detector_file,
                     resources.GetFile(kFaceDetectorTFLiteName));
    SetExternalFile(face_detector_file,
                    face_detector_graph_options->mutable_base_options()
                        ->mutable_model_asset(),
                    is_copy);
  }
  face_detector_graph_options->mutable_base_options()
      ->mutable_acceleration()
      ->CopyFrom(options->base_options().acceleration());
  face_detector_graph_options->mutable_base_options()->set_use_stream_mode(
      options->base_options().use_stream_mode());
  auto* face_landmarks_detector_graph_options =
      options->mutable_face_landmarker_graph_options()
          ->mutable_face_landmarks_detector_graph_options();
  if (!face_landmarks_detector_graph_options->base_options()
           .has_model_asset()) {
    ASSIGN_OR_RETURN(const auto face_landmarks_detector_file,
                     resources.GetFile(kFaceLandmarksDetectorTFLiteName));
    SetExternalFile(
        face_landmarks_detector_file,
        face_landmarks_detector_graph_options->mutable_base_options()
            ->mutable_model_asset(),
        is_copy);
  }
  face_landmarks_detector_graph_options->mutable_base_options()
      ->mutable_acceleration()
      ->CopyFrom(options->base_options().acceleration());
  face_landmarks_detector_graph_options->mutable_base_options()
      ->set_use_stream_mode(options->base_options().use_stream_mode());

  ASSIGN_OR_RETURN(const auto face_stylizer_file,
                   resources.GetFile(kFaceStylizerTFLiteName));
  SetExternalFile(face_stylizer_file, face_stylizer_external_file, is_copy);
  return absl::OkStatus();
}

void ConfigureSplitNormalizedLandmarkListVectorCalculator(
    mediapipe::SplitVectorCalculatorOptions* options) {
  auto* vector_range = options->add_ranges();
  vector_range->set_begin(0);
  vector_range->set_end(1);
  options->set_element_only(true);
}

void ConfigureLandmarksToDetectionCalculator(
    LandmarksToDetectionCalculatorOptions* options) {
  // left eye
  options->add_selected_landmark_indices(33);
  // left eye
  options->add_selected_landmark_indices(133);
  // right eye
  options->add_selected_landmark_indices(263);
  // right eye
  options->add_selected_landmark_indices(362);
  // mouth
  options->add_selected_landmark_indices(61);
  // mouth
  options->add_selected_landmark_indices(291);
}

void ConfigureTensorsToImageCalculator(
    const ImageToTensorCalculatorOptions& image_to_tensor_options,
    TensorsToImageCalculatorOptions* tensors_to_image_options) {
  tensors_to_image_options->set_gpu_origin(mediapipe::GpuOrigin_Mode_TOP_LEFT);
  if (image_to_tensor_options.has_output_tensor_float_range()) {
    auto* mutable_range =
        tensors_to_image_options->mutable_input_tensor_float_range();
    // TODO: Make the float range flexiable.
    mutable_range->set_min(0);
    mutable_range->set_max(1);
  } else if (image_to_tensor_options.has_output_tensor_uint_range()) {
    auto* mutable_range =
        tensors_to_image_options->mutable_input_tensor_uint_range();
    const auto& reference_range =
        image_to_tensor_options.output_tensor_uint_range();
    mutable_range->set_min(reference_range.min());
    mutable_range->set_max(reference_range.max());
  }
}

}  // namespace

// A "mediapipe.tasks.vision.face_stylizer.FaceStylizerGraph" performs face
// stylization on the detected face image.
//
// Inputs:
//   IMAGE - Image
//     Image to perform face stylization on.
//   NORM_RECT - NormalizedRect @Optional
//     Describes region of image to perform classification on.
//     @Optional: rect covering the whole image is used if not specified.
//
// Outputs:
//   IMAGE - mediapipe::Image
//     The face stylization output image.
//
// Example:
// node {
//   calculator: "mediapipe.tasks.vision.face_stylizer.FaceStylizerGraph"
//   input_stream: "IMAGE:image_in"
//   input_stream: "NORM_RECT:norm_rect"
//   output_stream: "IMAGE:image_out"
//   output_stream: "STYLIZED_IMAGE:stylized_image"
//   options {
//     [mediapipe.tasks.vision.face_stylizer.proto.FaceStylizerGraphOptions.ext]
//     {
//       base_options {
//         model_asset {
//           file_name: "face_stylizer.task"
//         }
//       }
//     }
//   }
// }
class FaceStylizerGraph : public core::ModelTaskGraph {
 public:
  absl::StatusOr<CalculatorGraphConfig> GetConfig(
      SubgraphContext* sc) override {
    ASSIGN_OR_RETURN(
        const auto* model_asset_bundle_resources,
        CreateModelAssetBundleResources<FaceStylizerGraphOptions>(sc));
    // Copies the file content instead of passing the pointer of file in
    // memory if the subgraph model resource service is not available.
    auto face_stylizer_external_file = absl::make_unique<ExternalFile>();
    MP_RETURN_IF_ERROR(SetSubTaskBaseOptions(
        *model_asset_bundle_resources,
        sc->MutableOptions<FaceStylizerGraphOptions>(),
        face_stylizer_external_file.get(),
        !sc->Service(::mediapipe::tasks::core::kModelResourcesCacheService)
             .IsAvailable()));
    Graph graph;
    ASSIGN_OR_RETURN(
        auto face_landmark_lists,
        BuildFaceLandmarkerGraph(
            sc->MutableOptions<FaceStylizerGraphOptions>()
                ->mutable_face_landmarker_graph_options(),
            graph[Input<Image>(kImageTag)],
            graph[Input<NormalizedRect>::Optional(kNormRectTag)], graph));
    ASSIGN_OR_RETURN(
        const auto* model_resources,
        CreateModelResources(sc, std::move(face_stylizer_external_file)));
    ASSIGN_OR_RETURN(
        auto output_streams,
        BuildFaceStylizerGraph(sc->Options<FaceStylizerGraphOptions>(),
                               *model_resources, graph[Input<Image>(kImageTag)],
                               face_landmark_lists, graph));
    output_streams.stylized_image >> graph[Output<Image>(kStylizedImageTag)];
    output_streams.original_image >> graph[Output<Image>(kImageTag)];
    return graph.GetConfig();
  }

 private:
  absl::StatusOr<Source<std::vector<NormalizedLandmarkList>>>
  BuildFaceLandmarkerGraph(FaceLandmarkerGraphOptions* face_landmarker_options,
                           Source<Image> image_in,
                           Source<NormalizedRect> norm_rect_in, Graph& graph) {
    auto& landmarker_graph = graph.AddNode(
        "mediapipe.tasks.vision.face_landmarker.FaceLandmarkerGraph");

    if (face_landmarker_options->face_detector_graph_options()
            .has_num_faces() &&
        face_landmarker_options->face_detector_graph_options().num_faces() !=
            1) {
      return CreateStatusWithPayload(
          absl::StatusCode::kInvalidArgument,
          "Face stylizer currently only supports one face.",
          MediaPipeTasksStatus::kInvalidArgumentError);
    }
    face_landmarker_options->mutable_face_detector_graph_options()
        ->set_num_faces(1);
    image_in >> landmarker_graph.In(kImageTag);
    norm_rect_in >> landmarker_graph.In(kNormRectTag);
    landmarker_graph.GetOptions<FaceLandmarkerGraphOptions>().Swap(
        face_landmarker_options);
    return landmarker_graph.Out(kNormLandmarksTag)
        .Cast<std::vector<NormalizedLandmarkList>>();
  }

  absl::StatusOr<FaceStylizerOutputStreams> BuildFaceStylizerGraph(
      const FaceStylizerGraphOptions& task_options,
      const ModelResources& model_resources, Source<Image> image_in,
      Source<std::vector<NormalizedLandmarkList>> face_landmark_lists,
      Graph& graph) {
    auto& split_face_landmark_list =
        graph.AddNode("SplitNormalizedLandmarkListVectorCalculator");
    ConfigureSplitNormalizedLandmarkListVectorCalculator(
        &split_face_landmark_list
             .GetOptions<mediapipe::SplitVectorCalculatorOptions>());
    face_landmark_lists >> split_face_landmark_list.In("");
    auto face_landmarks = split_face_landmark_list.Out("");

    auto& landmarks_to_detection =
        graph.AddNode("LandmarksToDetectionCalculator");
    ConfigureLandmarksToDetectionCalculator(
        &landmarks_to_detection
             .GetOptions<LandmarksToDetectionCalculatorOptions>());
    face_landmarks >> landmarks_to_detection.In(kNormLandmarksTag);
    auto face_detection = landmarks_to_detection.Out(kDetectionTag);

    auto& get_image_size = graph.AddNode("ImagePropertiesCalculator");
    image_in >> get_image_size.In(kImageTag);
    auto image_size = get_image_size.Out(kSizeTag);
    auto& face_to_rect = graph.AddNode("FaceToRectCalculator");
    face_detection >> face_to_rect.In(kDetectionTag);
    image_size >> face_to_rect.In(kImageSizeTag);
    auto face_rect = face_to_rect.Out(kNormRectTag);
    // Adds preprocessing calculators and connects them to the graph input image
    // stream.
    auto& preprocessing = graph.AddNode(
        "mediapipe.tasks.components.processors.ImagePreprocessingGraph");
    bool use_gpu =
        components::processors::DetermineImagePreprocessingGpuBackend(
            task_options.base_options().acceleration());
    MP_RETURN_IF_ERROR(components::processors::ConfigureImagePreprocessingGraph(
        model_resources, use_gpu,
        &preprocessing.GetOptions<tasks::components::processors::proto::
                                      ImagePreprocessingGraphOptions>()));
    auto& image_to_tensor_options =
        *preprocessing
             .GetOptions<components::processors::proto::
                             ImagePreprocessingGraphOptions>()
             .mutable_image_to_tensor_options();
    image_to_tensor_options.set_keep_aspect_ratio(true);
    image_to_tensor_options.set_border_mode(
        mediapipe::ImageToTensorCalculatorOptions::BORDER_ZERO);
    image_in >> preprocessing.In(kImageTag);
    face_rect >> preprocessing.In(kNormRectTag);
    auto preprocessed_tensors = preprocessing.Out(kTensorsTag);
    auto transform_matrix = preprocessing.Out(kMatrixTag);

    // Adds inference subgraph and connects its input stream to the output
    // tensors produced by the ImageToTensorCalculator.
    auto& inference = AddInference(
        model_resources, task_options.base_options().acceleration(), graph);
    preprocessed_tensors >> inference.In(kTensorsTag);
    auto model_output_tensors =
        inference.Out(kTensorsTag).Cast<std::vector<Tensor>>();

    auto& tensors_to_image =
        graph.AddNode("mediapipe.tasks.TensorsToImageCalculator");
    ConfigureTensorsToImageCalculator(
        image_to_tensor_options,
        &tensors_to_image.GetOptions<TensorsToImageCalculatorOptions>());
    model_output_tensors >> tensors_to_image.In(kTensorsTag);
    auto tensor_image = tensors_to_image.Out(kImageTag);

    auto& inverse_matrix = graph.AddNode("InverseMatrixCalculator");
    transform_matrix >> inverse_matrix.In(kMatrixTag);
    auto inverse_transform_matrix = inverse_matrix.Out(kMatrixTag);

    auto& warp_affine = graph.AddNode("WarpAffineCalculator");
    auto& warp_affine_options =
        warp_affine.GetOptions<WarpAffineCalculatorOptions>();
    warp_affine_options.set_border_mode(
        WarpAffineCalculatorOptions::BORDER_ZERO);
    warp_affine_options.set_gpu_origin(mediapipe::GpuOrigin_Mode_TOP_LEFT);
    tensor_image >> warp_affine.In(kImageTag);
    inverse_transform_matrix >> warp_affine.In(kMatrixTag);
    image_size >> warp_affine.In(kOutputSizeTag);
    auto image_to_crop = warp_affine.Out(kImageTag);

    // The following calculators are for cropping and resizing the output image
    // based on the roi and the model output size. As the WarpAffineCalculator
    // rotates the image based on the transform matrix, the rotation info in the
    // rect proto is stripped to prevent the ImageCroppingCalculator from
    // performing extra rotation.
    auto& strip_rotation =
        graph.AddNode("mediapipe.tasks.StripRotationCalculator");
    face_rect >> strip_rotation.In(kNormRectTag);
    auto norm_rect_no_rotation = strip_rotation.Out(kNormRectTag);
    auto& from_image = graph.AddNode("FromImageCalculator");
    image_to_crop >> from_image.In(kImageTag);
    auto& image_cropping = graph.AddNode("ImageCroppingCalculator");
    auto& image_cropping_opts =
        image_cropping.GetOptions<ImageCroppingCalculatorOptions>();
    image_cropping_opts.set_output_max_width(
        image_to_tensor_options.output_tensor_width());
    image_cropping_opts.set_output_max_height(
        image_to_tensor_options.output_tensor_height());
    norm_rect_no_rotation >> image_cropping.In(kNormRectTag);
    auto& to_image = graph.AddNode("ToImageCalculator");
    // ImageCroppingCalculator currently doesn't support mediapipe::Image, the
    // graph selects its cpu or gpu path based on the image preprocessing
    // backend.
    if (use_gpu) {
      from_image.Out(kImageGpuTag) >> image_cropping.In(kImageGpuTag);
      image_cropping.Out(kImageGpuTag) >> to_image.In(kImageGpuTag);
    } else {
      from_image.Out(kImageCpuTag) >> image_cropping.In(kImageTag);
      image_cropping.Out(kImageTag) >> to_image.In(kImageCpuTag);
    }

    return {{/*stylized_image=*/to_image.Out(kImageTag).Cast<Image>(),
             /*original_image=*/preprocessing.Out(kImageTag).Cast<Image>()}};
  }
};

// clang-format off
REGISTER_MEDIAPIPE_GRAPH(
  ::mediapipe::tasks::vision::face_stylizer::FaceStylizerGraph);  // NOLINT
// clang-format on

}  // namespace face_stylizer
}  // namespace vision
}  // namespace tasks
}  // namespace mediapipe
