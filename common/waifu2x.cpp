#include "waifu2x.h"
#include <caffe/caffe.hpp>
#include <cudnn.h>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <rapidjson/document.h>
#include <tclap/CmdLine.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <chrono>
#include <cuda_runtime.h>

#if defined(WIN32) || defined(WIN64)
#include <Windows.h>

#undef LoadImage
#endif

#ifdef _MSC_VER
#ifdef _DEBUG
#pragma comment(lib, "libcaffed.lib")
#pragma comment(lib, "libprotobufd.lib")
#else
#pragma comment(lib, "libcaffe.lib")
#pragma comment(lib, "libprotobuf.lib")
#endif
#pragma comment(lib, "libprotoc.lib")
#endif

// 入力画像のオフセット
const int offset = 0;
// srcnn.prototxtで定義されたレイヤーの数
const int layer_num = 7;

const int ConvertMode = CV_RGB2YUV;
const int ConvertInverseMode = CV_YUV2RGB;

// 最低限必要なCUDAドライバーのバージョン
const int MinCudaDriverVersion = 6050;

static std::once_flag waifu2x_once_flag;
static std::once_flag waifu2x_cudnn_once_flag;
static std::once_flag waifu2x_cuda_once_flag;

#ifndef CUDA_CHECK_WAIFU2X
#define CUDA_CHECK_WAIFU2X(condition) \
 do { \
    cudaError_t error = condition; \
    if(error != cudaSuccess) throw error; \
 } while (0)
#endif

#define CUDA_HOST_SAFE_FREE(ptr) \
	do { \
		if (ptr) { \
			cudaFreeHost(ptr); \
			ptr = nullptr; \
		} \
	} while (0)

#define SAFE_DELETE_WAIFU2X(ptr) \
	do { \
		if (ptr) { \
			delete [] ptr; \
			ptr = nullptr; \
		} \
	} while (0)

Waifu2x::Waifu2x() : is_inited(false), isCuda(false), input_block(nullptr), dummy_data(nullptr), output_block(nullptr)
{
}

Waifu2x::~Waifu2x()
{
	destroy();
}

// cuDNNが使えるかチェック。現状Windowsのみ
Waifu2x::eWaifu2xcuDNNError Waifu2x::can_use_cuDNN()
{
	static eWaifu2xcuDNNError cuDNNFlag = eWaifu2xcuDNNError_NotFind;
	std::call_once(waifu2x_cudnn_once_flag, [&]()
	{
#if defined(WIN32) || defined(WIN64)
		HMODULE hModule = LoadLibrary(TEXT("cudnn64_65.dll"));
		if (hModule != NULL)
		{
			typedef cudnnStatus_t(__stdcall * cudnnCreateType)(cudnnHandle_t *);
			typedef cudnnStatus_t(__stdcall * cudnnDestroyType)(cudnnHandle_t);
			typedef uint64_t(__stdcall * cudnnGetVersionType)();

			cudnnCreateType cudnnCreateFunc = (cudnnCreateType)GetProcAddress(hModule, "cudnnCreate");
			cudnnDestroyType cudnnDestroyFunc = (cudnnDestroyType)GetProcAddress(hModule, "cudnnDestroy");
			cudnnGetVersionType cudnnGetVersionFunc = (cudnnGetVersionType)GetProcAddress(hModule, "cudnnGetVersion");
			if (cudnnCreateFunc != nullptr && cudnnDestroyFunc != nullptr && cudnnGetVersionFunc != nullptr)
			{
				if (cudnnGetVersionFunc() >= 2000)
				{
					cudnnHandle_t h;
					if (cudnnCreateFunc(&h) == CUDNN_STATUS_SUCCESS)
					{
						if (cudnnDestroyFunc(h) == CUDNN_STATUS_SUCCESS)
							cuDNNFlag = eWaifu2xcuDNNError_OK;
						else
							cuDNNFlag = eWaifu2xcuDNNError_CannotCreate;
					}
					else
						cuDNNFlag = eWaifu2xcuDNNError_CannotCreate;
				}
				else
					cuDNNFlag = eWaifu2xcuDNNError_OldVersion;
			}
			else
				cuDNNFlag = eWaifu2xcuDNNError_NotFind;

			FreeLibrary(hModule);
		}
#endif
	});

	return cuDNNFlag;
}

// CUDAが使えるかチェック
Waifu2x::eWaifu2xCudaError Waifu2x::can_use_CUDA()
{
	static eWaifu2xCudaError CudaFlag = eWaifu2xCudaError_NotFind;
	std::call_once(waifu2x_cuda_once_flag, [&]()
	{
		int driverVersion = 0;
		if (cudaDriverGetVersion(&driverVersion) == cudaSuccess)
		{
			if (driverVersion > 0)
			{
				int runtimeVersion;
				if (cudaRuntimeGetVersion(&runtimeVersion) == cudaSuccess)
				{
					if (runtimeVersion >= MinCudaDriverVersion && driverVersion >= runtimeVersion)
						CudaFlag = eWaifu2xCudaError_OK;
					else
						CudaFlag = eWaifu2xCudaError_OldVersion;
				}
				else
					CudaFlag = eWaifu2xCudaError_NotFind;
			}
			else
				CudaFlag = eWaifu2xCudaError_NotFind;
		}
		else
			CudaFlag = eWaifu2xCudaError_NotFind;
	});

	return CudaFlag;
}

// 画像を読み込んで値を0.0f〜1.0fの範囲に変換
Waifu2x::eWaifu2xError Waifu2x::LoadImage(cv::Mat &float_image, const std::string &input_file)
{
	cv::Mat original_image = cv::imread(input_file, cv::IMREAD_UNCHANGED);
	if (original_image.empty())
		return eWaifu2xError_FailedOpenInputFile;

	cv::Mat convert;
	original_image.convertTo(convert, CV_32F, 1.0 / 255.0);
	original_image.release();

	if (convert.channels() == 1)
		cv::cvtColor(convert, convert, cv::COLOR_GRAY2BGR);
	else if (convert.channels() == 4)
	{
		// アルファチャンネル付きだったら背景を1(白)として画像合成する

		std::vector<cv::Mat> planes;
		cv::split(convert, planes);

		cv::Mat w2 = planes[3];
		cv::Mat w1 = 1.0 - planes[3];

		planes[0] = planes[0].mul(w2) + w1;
		planes[1] = planes[1].mul(w2) + w1;
		planes[2] = planes[2].mul(w2) + w1;

		cv::merge(planes, convert);
	}

	float_image = convert;

	return eWaifu2xError_OK;
}

// 画像から輝度の画像を取り出す
Waifu2x::eWaifu2xError Waifu2x::CreateBrightnessImage(const cv::Mat &float_image, cv::Mat &im)
{
	cv::Mat converted_color;
	cv::cvtColor(float_image, converted_color, ConvertMode);

	std::vector<cv::Mat> planes;
	cv::split(converted_color, planes);

	im = planes[0];
	planes.clear();

	return eWaifu2xError_OK;
}

// 入力画像の(Photoshopでいう)キャンバスサイズをoutput_sizeの倍数に変更
// 画像は左上配置、余白はcv::BORDER_REPLICATEで埋める
Waifu2x::eWaifu2xError Waifu2x::PaddingImage(const cv::Mat &input, cv::Mat &output)
{
	const auto h_blocks = (int)floor(input.size().width / output_size) + (input.size().width % output_size == 0 ? 0 : 1);
	const auto w_blocks = (int)floor(input.size().height / output_size) + (input.size().height % output_size == 0 ? 0 : 1);
	const auto height = offset + h_blocks * output_size + offset;
	const auto width = offset + w_blocks * output_size + offset;
	const auto pad_h1 = offset;
	const auto pad_w1 = offset;
	const auto pad_h2 = (height - offset) - input.size().width;
	const auto pad_w2 = (width - offset) - input.size().height;

	cv::copyMakeBorder(input, output, pad_w1, pad_w2, pad_h1, pad_h2, cv::BORDER_REPLICATE);

	return eWaifu2xError_OK;
}

// 画像をcv::INTER_NEARESTで二倍に拡大して、PaddingImage()でパディングする
Waifu2x::eWaifu2xError Waifu2x::Zoom2xAndPaddingImage(const cv::Mat &input, cv::Mat &output, cv::Size_<int> &zoom_size)
{
	zoom_size = input.size();
	zoom_size.width *= 2;
	zoom_size.height *= 2;

	cv::Mat zoom_image;
	cv::resize(input, zoom_image, zoom_size, 0.0, 0.0, cv::INTER_NEAREST);

	return PaddingImage(zoom_image, output);
}

// 入力画像をzoom_sizeの大きさにcv::INTER_CUBICで拡大し、色情報のみを残す
Waifu2x::eWaifu2xError Waifu2x::CreateZoomColorImage(const cv::Mat &float_image, const cv::Size_<int> &zoom_size, std::vector<cv::Mat> &cubic_planes)
{
	cv::Mat zoom_cubic_image;
	cv::resize(float_image, zoom_cubic_image, zoom_size, 0.0, 0.0, cv::INTER_CUBIC);

	cv::Mat converted_cubic_image;
	cv::cvtColor(zoom_cubic_image, converted_cubic_image, ConvertMode);
	zoom_cubic_image.release();

	cv::split(converted_cubic_image, cubic_planes);
	converted_cubic_image.release();

	// このY成分は使わないので解放
	cubic_planes[0].release();

	return eWaifu2xError_OK;
}

// 学習したパラメータをファイルから読み込む
Waifu2x::eWaifu2xError Waifu2x::LoadParameter(boost::shared_ptr<caffe::Net<float>> net, const std::string &param_path)
{
	const std::string caffemodel_path = param_path + ".caffemodel";

	FILE *fp = fopen(caffemodel_path.c_str(), "rb");
	const bool isModelExist = fp != nullptr;
	if (fp) fclose(fp);

	caffe::NetParameter param;
	if (isModelExist && caffe::ReadProtoFromBinaryFile(caffemodel_path, &param))
		net->CopyTrainedLayersFrom(param);
	else
	{
		rapidjson::Document d;
		std::vector<char> jsonBuf;

		try
		{
			FILE *fp = fopen(param_path.c_str(), "rb");
			if (fp == nullptr)
				return eWaifu2xError_FailedOpenModelFile;

			fseek(fp, 0, SEEK_END);
			const auto size = ftell(fp);
			fseek(fp, 0, SEEK_SET);

			jsonBuf.resize(size + 1);
			fread(jsonBuf.data(), 1, size, fp);

			fclose(fp);

			jsonBuf[jsonBuf.size() - 1] = '\0';

			d.Parse(jsonBuf.data());
		}
		catch (...)
		{
			return eWaifu2xError_FailedParseModelFile;
		}

		std::vector<boost::shared_ptr<caffe::Layer<float>>> list;
		auto &v = net->layers();
		for (auto &l : v)
		{
			auto lk = l->type();
			auto &bv = l->blobs();
			if (bv.size() > 0)
				list.push_back(l);
		}

		try
		{
			std::vector<float> weightList;
			std::vector<float> biasList;

			int count = 0;
			for (auto it = d.Begin(); it != d.End(); ++it)
			{
				const auto &weight = (*it)["weight"];
				const auto nInputPlane = (*it)["nInputPlane"].GetInt();
				const auto nOutputPlane = (*it)["nOutputPlane"].GetInt();
				const auto kW = (*it)["kW"].GetInt();
				const auto &bias = (*it)["bias"];

				auto leyer = list[count];

				auto &b0 = leyer->blobs()[0];
				auto &b1 = leyer->blobs()[1];

				float *b0Ptr = nullptr;
				float *b1Ptr = nullptr;

				if (caffe::Caffe::mode() == caffe::Caffe::CPU)
				{
					b0Ptr = b0->mutable_cpu_data();
					b1Ptr = b1->mutable_cpu_data();
				}
				else
				{
					b0Ptr = b0->mutable_gpu_data();
					b1Ptr = b1->mutable_gpu_data();
				}

				const auto WeightSize1 = weight.Size();
				const auto WeightSize2 = weight[0].Size();
				const auto KernelHeight = weight[0][0].Size();
				const auto KernelWidth = weight[0][0][0].Size();

				if (!(b0->count() == WeightSize1 * WeightSize2 * KernelHeight * KernelWidth))
					return eWaifu2xError_FailedConstructModel;

				if (!(b1->count() == bias.Size()))
					return eWaifu2xError_FailedConstructModel;

				weightList.resize(0);
				biasList.resize(0);

				size_t weightCount = 0;
				for (auto it2 = weight.Begin(); it2 != weight.End(); ++it2)
				{
					for (auto it3 = (*it2).Begin(); it3 != (*it2).End(); ++it3)
					{
						for (auto it4 = (*it3).Begin(); it4 != (*it3).End(); ++it4)
						{
							for (auto it5 = (*it4).Begin(); it5 != (*it4).End(); ++it5)
								weightList.push_back((float)it5->GetDouble());
						}
					}
				}

				caffe::caffe_copy(b0->count(), weightList.data(), b0Ptr);

				for (auto it2 = bias.Begin(); it2 != bias.End(); ++it2)
					biasList.push_back((float)it2->GetDouble());

				caffe::caffe_copy(b1->count(), biasList.data(), b1Ptr);

				count++;
			}

			net->ToProto(&param);

			caffe::WriteProtoToBinaryFile(param, caffemodel_path);
		}
		catch (...)
		{
			return eWaifu2xError_FailedConstructModel;
		}
	}

	return eWaifu2xError_OK;
}

// モデルファイルからネットワークを構築
// processでcudnnが指定されなかった場合はcuDNNが呼び出されないように変更する
Waifu2x::eWaifu2xError Waifu2x::ConstractNet(boost::shared_ptr<caffe::Net<float>> &net, const std::string &model_path, const std::string &process)
{
	caffe::NetParameter param;
	if (!caffe::ReadProtoFromTextFile(model_path, &param))
		return eWaifu2xError_FailedOpenModelFile;

	param.mutable_state()->set_phase(caffe::TEST);

	for (int i = 0; i < param.layer_size(); i++)
	{
		caffe::LayerParameter *layer_param = param.mutable_layer(i);
		const std::string& type = layer_param->type();
		if (type == "Convolution")
		{
			if (process == "cudnn")
				layer_param->mutable_convolution_param()->set_engine(caffe::ConvolutionParameter_Engine_CUDNN);
			else
				layer_param->mutable_convolution_param()->set_engine(caffe::ConvolutionParameter_Engine_CAFFE);
		}
		else if (type == "ReLU")
		{
			if (process == "cudnn")
				layer_param->mutable_relu_param()->set_engine(caffe::ReLUParameter_Engine_CUDNN);
			else
				layer_param->mutable_relu_param()->set_engine(caffe::ReLUParameter_Engine_CAFFE);
		}
		else if (type == "MemoryData")
		{
			if (layer_param->mutable_memory_data_param()->width() == original_width_height && layer_param->mutable_memory_data_param()->height() == original_width_height)
			{
				layer_param->mutable_memory_data_param()->set_width(input_block_size);
				layer_param->mutable_memory_data_param()->set_height(input_block_size);
			}
		}
	}

	net = boost::shared_ptr<caffe::Net<float>>(new caffe::Net<float>(param));

	return eWaifu2xError_OK;
}

// ネットワークを使って画像を再構築する
Waifu2x::eWaifu2xError Waifu2x::ReconstructImage(boost::shared_ptr<caffe::Net<float>> net, cv::Mat &im)
{
	const auto Height = im.size().height;
	const auto Width = im.size().width;
	const auto Line = im.step1();

	assert(Width % output_size == 0);
	assert(Height % output_size == 0);

	assert(im.channels() == 1);

	cv::Mat outim(im.rows, im.cols, im.type());

	// float *imptr = (float *)im.data;
	float *imptr = (float *)outim.data;

	try
	{
		const auto input_layer =
			boost::dynamic_pointer_cast<caffe::MemoryDataLayer<float>>(
			net->layer_by_name("image_input_layer"));
		assert(input_layer);

		const auto conv7_layer =
			boost::dynamic_pointer_cast<caffe::ConvolutionLayer<float>>(
			net->layer_by_name("conv7_layer"));
		assert(conv7_layer);

		input_layer->set_batch_size(batch_size);

		const int WidthNum = Width / output_size;
		const int HeightNum = Height / output_size;

		const int BlockNum = WidthNum * HeightNum;

		const int input_block_plane_size = input_block_size * input_block_size;
		const int output_block_plane_size = output_block_size * output_block_size;

		const int output_padding = inner_padding + outer_padding - layer_num;

		// 画像は(消費メモリの都合上)output_size*output_sizeに分けて再構築する
		for (int num = 0; num < BlockNum; num += batch_size)
		{
			const int processNum = (BlockNum - num) >= batch_size ? batch_size : BlockNum - num;

			if (processNum < batch_size)
				input_layer->set_batch_size(processNum);

			for (int n = 0; n < processNum; n++)
			{
				const int wn = (num + n) % WidthNum;
				const int hn = (num + n) / WidthNum;

				const int w = wn * output_size;
				const int h = hn * output_size;

				if (w + crop_size <= Width && h + crop_size <= Height)
				{
					int x, y;
					x = w - inner_padding;
					y = h - inner_padding;

					int width, height;

					width = crop_size + inner_padding * 2;
					height = crop_size + inner_padding * 2;

					int top, bottom, left, right;

					top = outer_padding;
					bottom = outer_padding;
					left = outer_padding;
					right = outer_padding;

					if (x < 0)
					{
						left += -x;
						width -= -x;
						x = 0;
					}

					if (x + width > Width)
					{
						right += (x + width) - Width;
						width = Width - x;
					}

					if (y < 0)
					{
						top += -y;
						height -= -y;
						y = 0;
					}

					if (y + height > Height)
					{
						bottom += (y + height) - Height;
						height = Height - y;
					}

					cv::Mat someimg = im(cv::Rect(x, y, width, height));

					cv::Mat someimg_tmp;
					someimg.copyTo(someimg_tmp);
					someimg.release();

					cv::Mat someborderimg;
					// 画像を中央にパディング。余白はcv::BORDER_REPLICATEで埋める
					cv::copyMakeBorder(someimg_tmp, someborderimg, top, bottom, left, right, cv::BORDER_REPLICATE);
					someimg_tmp.release();

					// 画像を直列に変換
					{
						float *fptr = input_block + (input_block_plane_size * n);
						const float *uptr = (const float *)someborderimg.data;

						const auto Line = someborderimg.step1();

						if (input_block_size == Line)
							memcpy(fptr, uptr, input_block_size * input_block_size * sizeof(float));
						else
						{
							for (int i = 0; i < input_block_size; i++)
								memcpy(fptr + i * input_block_size, uptr + i * Line, input_block_size * sizeof(float));
						}
					}
				}
			}

			// ネットワークに画像を入力
			input_layer->Reset(input_block, dummy_data, input_block_plane_size * processNum);

			// 計算
			auto out = net->ForwardPrefilled(nullptr);

			auto b = out[0];

			assert(b->count() == output_block_plane_size * processNum);

			const float *ptr = nullptr;

			if (caffe::Caffe::mode() == caffe::Caffe::CPU)
				ptr = b->cpu_data();
			else
				ptr = b->gpu_data();

			caffe::caffe_copy(output_block_plane_size * processNum, ptr, output_block);

			for (int n = 0; n < processNum; n++)
			{
				const int wn = (num + n) % WidthNum;
				const int hn = (num + n) / WidthNum;

				const int w = wn * output_size;
				const int h = hn * output_size;

				const float *fptr = output_block + (output_block_plane_size * n);

				// 結果を入力画像にコピー(後に処理する部分とここで上書きする部分は被らないから、入力画像を上書きしても大丈夫)
				for (int i = 0; i < crop_size; i++)
					memcpy(imptr + (h + i) * Line + w, fptr + (i + output_padding) * output_block_size + output_padding, crop_size * sizeof(float));
			}
		}
	}
	catch (...)
	{
		return eWaifu2xError_FailedProcessCaffe;
	}

	im = outim;

	return eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::init(int argc, char** argv, const std::string &Mode, const int NoiseLevel, const double ScaleRatio, const std::string &ModelDir, const std::string &Process,
	const int CropSize, const int BatchSize)
{
	Waifu2x::eWaifu2xError ret;

	if (is_inited)
		return eWaifu2xError_OK;

	if (ScaleRatio <= 0.0)
		return eWaifu2xError_InvalidParameter;

	try
	{
		mode = Mode;
		noise_level = NoiseLevel;
		scale_ratio = ScaleRatio;
		model_dir = ModelDir;
		process = Process;

		crop_size = CropSize;
		batch_size = BatchSize;

		inner_padding = layer_num;
		outer_padding = 1;

		output_size = crop_size - offset * 2;
		input_block_size = crop_size + (inner_padding + outer_padding) * 2;
		original_width_height = 128 + layer_num * 2;

		output_block_size = crop_size + (inner_padding + outer_padding - layer_num) * 2;

		std::call_once(waifu2x_once_flag, [argc, argv]()
		{
			assert(argc >= 1);

			int tmpargc = 1;
			char* tmpargvv[] = { argv[0] };
			char** tmpargv = tmpargvv;
			// glog等の初期化
			caffe::GlobalInit(&tmpargc, &tmpargv);
		});

		const auto cuDNNCheckStartTime = std::chrono::system_clock::now();

		if (process == "gpu")
		{
			if (can_use_CUDA() != eWaifu2xCudaError_OK)
				return eWaifu2xError_FailedCudaCheck;
			// cuDNNが使えそうならcuDNNを使う
			else if (can_use_cuDNN() == eWaifu2xcuDNNError_OK)
				process = "cudnn";
		}

		const auto cuDNNCheckEndTime = std::chrono::system_clock::now();

		boost::filesystem::path mode_dir_path(model_dir);
		if (!mode_dir_path.is_absolute()) // model_dirが相対パスなら絶対パスに直す
		{
			// まずはカレントディレクトリ下にあるか探す
			mode_dir_path = boost::filesystem::absolute(model_dir);
			if (!boost::filesystem::exists(mode_dir_path) && argc >= 1) // 無かったらargv[0]から実行ファイルのあるフォルダを推定し、そのフォルダ下にあるか探す
			{
				boost::filesystem::path a0(argv[0]);
				if (a0.is_absolute())
					mode_dir_path = a0.branch_path() / model_dir;
			}
		}

		if (!boost::filesystem::exists(mode_dir_path))
			return eWaifu2xError_FailedOpenModelFile;

		if (process == "cpu")
		{
			caffe::Caffe::set_mode(caffe::Caffe::CPU);
			isCuda = false;
		}
		else
		{
			caffe::Caffe::set_mode(caffe::Caffe::GPU);
			isCuda = true;
		}

		if (mode == "noise" || mode == "noise_scale" || mode == "auto_scale")
		{
			const std::string model_path = (mode_dir_path / "srcnn.prototxt").string();
			const std::string param_path = (mode_dir_path / ("noise" + std::to_string(noise_level) + "_model.json")).string();

			ret = ConstractNet(net_noise, model_path, process);
			if (ret != eWaifu2xError_OK)
				return ret;

			ret = LoadParameter(net_noise, param_path);
			if (ret != eWaifu2xError_OK)
				return ret;
		}

		if (mode == "scale" || mode == "noise_scale" || mode == "auto_scale")
		{
			const std::string model_path = (mode_dir_path / "srcnn.prototxt").string();
			const std::string param_path = (mode_dir_path / "scale2.0x_model.json").string();

			ret = ConstractNet(net_scale, model_path, process);
			if (ret != eWaifu2xError_OK)
				return ret;

			ret = LoadParameter(net_scale, param_path);
			if (ret != eWaifu2xError_OK)
				return ret;
		}

		const int input_block_plane_size = input_block_size * input_block_size;
		const int output_block_plane_size = output_block_size * output_block_size;

		if (isCuda)
		{
			CUDA_CHECK_WAIFU2X(cudaHostAlloc(&input_block, sizeof(float) * input_block_plane_size * batch_size, cudaHostAllocWriteCombined));
			CUDA_CHECK_WAIFU2X(cudaHostAlloc(&dummy_data, sizeof(float) * input_block_plane_size * batch_size, cudaHostAllocWriteCombined));
			CUDA_CHECK_WAIFU2X(cudaHostAlloc(&output_block, sizeof(float) * output_block_plane_size * batch_size, cudaHostAllocDefault));
		}
		else
		{
			input_block = new float[input_block_plane_size * batch_size];
			dummy_data = new float[input_block_plane_size * batch_size];
			output_block = new float[output_block_plane_size * batch_size];
		}

		for (size_t i = 0; i < input_block_plane_size * batch_size; i++)
			dummy_data[i] = 0.0f;

		is_inited = true;
	}
	catch (...)
	{
		return eWaifu2xError_InvalidParameter;
	}

	return eWaifu2xError_OK;
}

void Waifu2x::destroy()
{
	net_noise.reset();
	net_scale.reset();

	if (isCuda)
	{
		CUDA_HOST_SAFE_FREE(input_block);
		CUDA_HOST_SAFE_FREE(dummy_data);
		CUDA_HOST_SAFE_FREE(output_block);
	}
	else
	{
		SAFE_DELETE_WAIFU2X(input_block);
		SAFE_DELETE_WAIFU2X(dummy_data);
		SAFE_DELETE_WAIFU2X(output_block);
	}

	is_inited = false;
}

Waifu2x::eWaifu2xError Waifu2x::waifu2x(const std::string &input_file, const std::string &output_file,
	const waifu2xCancelFunc cancel_func)
{
	Waifu2x::eWaifu2xError ret;

	if (!is_inited)
		return eWaifu2xError_NotInitialized;

	cv::Mat float_image;
	ret = LoadImage(float_image, input_file);
	if (ret != eWaifu2xError_OK)
		return ret;

	cv::Mat im;
	CreateBrightnessImage(float_image, im);

	cv::Size_<int> image_size = im.size();

	const boost::filesystem::path ip(input_file);
	const boost::filesystem::path ipext(ip.extension());

	const bool isJpeg = boost::iequals(ipext.string(), ".jpg") || boost::iequals(ipext.string(), ".jpeg");

	const bool isReconstructNoise = mode == "noise" || mode == "noise_scale" || (mode == "auto_scale" && isJpeg);
	const bool isReconstructScale = mode == "scale" || mode == "noise_scale";

	if (isReconstructNoise)
	{
		PaddingImage(im, im);

		ret = ReconstructImage(net_noise, im);
		if (ret != eWaifu2xError_OK)
			return ret;

		// パディングを取り払う
		im = im(cv::Rect(offset, offset, image_size.width, image_size.height));
	}

	if (cancel_func && cancel_func())
		return eWaifu2xError_Cancel;

	const int scale2 = ceil(log2(scale_ratio));
	const double shrinkRatio = scale_ratio / std::pow(2.0, (double)scale2);

	if (isReconstructScale)
	{
		bool isError = false;
		for (int i = 0; i < scale2; i++)
		{
			Zoom2xAndPaddingImage(im, im, image_size);

			ret = ReconstructImage(net_scale, im);
			if (ret != eWaifu2xError_OK)
				return ret;

			// パディングを取り払う
			im = im(cv::Rect(offset, offset, image_size.width, image_size.height));
		}
	}

	if (cancel_func && cancel_func())
		return eWaifu2xError_Cancel;

	// 再構築した輝度画像とCreateZoomColorImage()で作成した色情報をマージして通常の画像に変換し、書き込む

	std::vector<cv::Mat> color_planes;
	CreateZoomColorImage(float_image, image_size, color_planes);

	cv::Mat alpha;
	if (float_image.channels() == 4)
	{
		std::vector<cv::Mat> planes;
		cv::split(float_image, planes);
		alpha = planes[3];

		cv::resize(alpha, alpha, image_size, 0.0, 0.0, cv::INTER_CUBIC);
	}

	float_image.release();

	color_planes[0] = im;
	im.release();

	cv::Mat converted_image;
	cv::merge(color_planes, converted_image);
	color_planes.clear();

	cv::Mat process_image;
	cv::cvtColor(converted_image, process_image, ConvertInverseMode);
	converted_image.release();

	// アルファチャンネルがあったら、アルファを付加してカラーからアルファの影響を抜く
	if (!alpha.empty())
	{
		std::vector<cv::Mat> planes;
		cv::split(process_image, planes);
		process_image.release();

		planes.push_back(alpha);

		cv::Mat w2 = planes[3];

		planes[0] = (planes[0] - 1.0).mul(1.0 / w2) + 1.0;
		planes[1] = (planes[1] - 1.0).mul(1.0 / w2) + 1.0;
		planes[2] = (planes[2] - 1.0).mul(1.0 / w2) + 1.0;

		cv::merge(planes, process_image);
	}

	const cv::Size_<int> ns(image_size.width * shrinkRatio, image_size.height * shrinkRatio);
	if (image_size.width != ns.width || image_size.height != ns.height)
		cv::resize(process_image, process_image, ns, 0.0, 0.0, cv::INTER_LINEAR);

	cv::Mat write_iamge;
	process_image.convertTo(write_iamge, CV_8U, 255.0);
	process_image.release();

	if (!cv::imwrite(output_file, write_iamge))
		return eWaifu2xError_FailedOpenOutputFile;

	write_iamge.release();

	return eWaifu2xError_OK;
}

const std::string& Waifu2x::used_process() const
{
	return process;
}
