#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <thread>
#include <cuda_runtime.h>
#include "include/cuda_kernel.cuh"
#include "include/utils.h"

using namespace cv;
using namespace std;

// const float sobel_x_kernel[9] = {1, 0, -1, 2, 0, -2, 1, 0, -1};
const float sobel_x_kernel[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};

const float sobel_x_separable[3] = {1, 2, 1};
const float sobel_x_separable_2[3] = {1, 0, -1};

// const float sobel_y_kernel[9] = {1, 2, 1, 0, 0, 0, -1, -2, -1};
const float sobel_y_kernel[9] = {-1, -2, -1, 0, 0, 0, 1, 2, 1};

const float sobel_y_separable[3] = {1, 0, -1};
const float sobel_y_separable_2[3] = {1, 2, 1};
enum Mode
{
	// -H. Normal Harris Corner Detection
	HARRIS,
	// -S. Harris corner detection with Shi-Tomasi response function
	SHI_TOMASI,
	// -C. Canny Edge Detection with Otsu Thresholding
	CANNY,
	// -C -l=low -h=high. Canny Edge Detection with manual thresholding. Optional
	CANNY_MANUAL,
	// -C -g. Canny Edge Detection with GUI thresholding. Optional
	CANNY_GUI,
	// -O. Otsu thresholding method for image binarization
	OTSU_BIN,
	// -OP. Optical Flow naive implementation
	OPTICAL

};

void saveImage(float *img_d, size_t img_size_h, int height, int width, std::string filename)
{
	float *img_save = (float *)malloc(img_size_h);
	cudaMemcpy(img_save, img_d, img_size_h, cudaMemcpyDeviceToHost);
	cv::Mat img_gray(height, width, CV_32F, img_save);
	cv::imwrite(filename, img_gray);
}

/**
 * @brief Handles image processing: RGB to Gray, Gaussian Blur then Harris/ShiTomasi Corner Detection, Canny Edge Detection or Otsu binarization
 *
 * @param mode Execution mode. Can be HARRIS, SHI_TOMASI, CANNY, CANNY_MANUAL, CANNY_GUI, OTSU_BIN
 * @param filename Image filename
 * @param low_threshold Low threshold for Canny Edge Detection Manual mode
 * @param high_threshold High threshold for Canny Edge Detection Manual mode
 * @param from_video Flag to indicate if the image is taken from a video. Default is false
 * @param img_v If from_video is true, the image is passed as a cv::Mat. Default is empty
 */
void handleImage(enum Mode mode, std::string filename, int low_threshold, int high_threshold, bool from_video = false, cv::Mat img_v = cv::Mat())
{
	cv::Mat img;
	if (!from_video)
	{
		img = cv::imread(filename, cv::IMREAD_COLOR);

		if (img.empty())
		{
			std::cerr << "Error: Unable to load image." << std::endl;
			return;
		}
	}
	else
	{
		img = img_v;
	}
	cv::cvtColor(img, img, cv::COLOR_BGR2RGBA);

	// variable declarations
	int width = img.cols;
	int height = img.rows;
	int channels = img.channels();
	// printf("Channels: %d\n", channels);
	size_t img_size_h = width * height * channels * sizeof(unsigned char);
	size_t img_gray_size_h = width * height * sizeof(float);

	// printf("Image size: %d x %d \n", width, height);

	// device variable declarations
	uchar4 *img_d;
	float *img_gray_d;
	float *img_blurred_d;
	float *img_harris_d;
	float *img_sobel_x_d;
	float *img_sobel_y_d;

	// mallocs
	unsigned char *img_h = (unsigned char *)malloc(img_size_h);
	float *img_gray_h = (float *)malloc(img_gray_size_h);
	float *img_grayh2 = (float *)malloc(img_gray_size_h);
	float *img_harris_h = (float *)malloc(img_gray_size_h);

	// kernel devices
	float *sobel_x_kernel_d;
	float *sobel_x_separable_d;
	float *sobel_x_separable_2_d;
	float *sobel_y_kernel_d;
	float *sobel_y_separable_d;
	float *sobel_y_separable_2_d;

	auto start = std::chrono::high_resolution_clock::now();
	float *gaussian_kernel = computeGaussianKernel(FILTER_WIDTH, FILTER_SIGMA);
	float *gaussian_kernel_d;

	// data to host
	// memcpy(img_h, img.data, img_size_h);

	// cuda memory allocations

	cudaMalloc(&img_d, img_size_h);
	cudaMalloc(&img_gray_d, img_gray_size_h);
	cudaMalloc(&img_blurred_d, img_gray_size_h);
	cudaMalloc(&gaussian_kernel_d, FILTER_WIDTH * FILTER_WIDTH * sizeof(float));
	cudaMalloc(&sobel_x_kernel_d, 3 * 3 * sizeof(float));
	cudaMalloc(&sobel_x_separable_d, 3 * sizeof(float));
	cudaMalloc(&sobel_x_separable_2_d, 3 * sizeof(float));
	cudaMalloc(&sobel_y_kernel_d, 3 * 3 * sizeof(float));
	cudaMalloc(&sobel_y_separable_d, 3 * sizeof(float));
	cudaMalloc(&sobel_y_separable_2_d, 3 * sizeof(float));
	cudaMalloc(&img_sobel_x_d, img_gray_size_h);
	cudaMalloc(&img_sobel_y_d, img_gray_size_h);
	cudaMalloc(&img_harris_d, img_gray_size_h);

	// cudamemcpys
	// cudaMemcpy(img_d, img_h, img_size_h, cudaMemcpyHostToDevice);
	cudaHostRegister(img.data, img_size_h, cudaHostRegisterPortable);
	cudaMemcpy(img_d, img.data, img_size_h, cudaMemcpyHostToDevice);
	cudaMemcpy(gaussian_kernel_d, gaussian_kernel, FILTER_WIDTH * FILTER_WIDTH * sizeof(float), cudaMemcpyHostToDevice);

	cudaMemcpy(sobel_x_kernel_d, sobel_x_kernel, 3 * 3 * sizeof(float), cudaMemcpyHostToDevice);
	cudaMemcpy(sobel_y_kernel_d, sobel_y_kernel, 3 * 3 * sizeof(float), cudaMemcpyHostToDevice);

	cudaMemcpy(sobel_x_separable_d, sobel_x_separable, 3 * sizeof(float), cudaMemcpyHostToDevice);
	cudaMemcpy(sobel_x_separable_2_d, sobel_x_separable_2, 3 * sizeof(float), cudaMemcpyHostToDevice);
	cudaMemcpy(sobel_y_separable_d, sobel_y_separable, 3 * sizeof(float), cudaMemcpyHostToDevice);
	cudaMemcpy(sobel_y_separable_2_d, sobel_y_separable_2, 3 * sizeof(float), cudaMemcpyHostToDevice);

	// Commong operations for all modes(except for OTSU_BIN)

	// RGB to Gray
	rgbToGrayKernelWrap(img_d, img_gray_d, width, height);

	// Apply Gaussian Blur to grayscale image
	convolutionGPUWrap(img_blurred_d, img_gray_d, width, height, gaussian_kernel_d, FILTER_WIDTH);

	// Sobel X
	convolutionGPUWrap(img_sobel_x_d, img_blurred_d, width, height, sobel_x_kernel_d, 3);
	// separableConvolutionKernelWrap(img_blurred_d, img_sobel_x_d, width, height, sobel_x_separable_2_d, sobel_x_separable_d, 3);
	// float *img_sobel_x_h = (float *)malloc(img_gray_size_h);
	// cudaMemcpy(img_sobel_x_h, img_sobel_x_d, img_gray_size_h, cudaMemcpyDeviceToHost);
	// saveImage(height, width, img_sobel_x_h, "debug/sobel_x_cuda.jpg");

	// Sobel Y
	convolutionGPUWrap(img_sobel_y_d, img_blurred_d, width, height, sobel_y_kernel_d, 3);
	// separableConvolutionKernelWrap(img_blurred_d, img_sobel_y_d, width, height, sobel_x_separable_d, sobel_x_separable_2_d, 3);
	// float *img_sobel_y_h = (float *)malloc(img_gray_size_h);
	// cudaMemcpy(img_sobel_y_h, img_sobel_y_d, img_gray_size_h, cudaMemcpyDeviceToHost);
	// saveImage(height, width, img_sobel_y_h, "debug/sobel_y_cuda.jpg");

	// Exeuting the CV task based on the mode
	switch (mode)
	{
	case HARRIS:

		// harrisCornerDetector(&img, img_sobel_x_d, img_sobel_y_d, width, height, K, ALPHA, gaussian_kernel_d, FILTER_WIDTH, false);
		harrisMainKernelWrap((uchar4 *)img.data, img_d, img_sobel_x_d, img_sobel_y_d, width, height, K, ALPHA, gaussian_kernel_d, FILTER_WIDTH, false, nullptr);
		break;
	case SHI_TOMASI:
		// harrisCornerDetector(&img, img_sobel_x_d, img_sobel_y_d, width, height, K, ALPHA, gaussian_kernel_d, FILTER_WIDTH, true);
		harrisMainKernelWrap((uchar4 *)img.data, img_d, img_sobel_x_d, img_sobel_y_d, width, height, K, ALPHA, gaussian_kernel_d, FILTER_WIDTH, true, nullptr);
		break;
	case CANNY:
		high_threshold = otsuThreshold(img_blurred_d, width, height);
		low_threshold = high_threshold / 2;
		cannyMainKernelWrap((uchar4 *)img.data, img_d, img_sobel_x_d, img_sobel_y_d, width, height, low_threshold, high_threshold, gaussian_kernel_d, FILTER_WIDTH, from_video);
		break;
	case CANNY_MANUAL:
		cannyMainKernelWrap((uchar4 *)img.data, img_d, img_sobel_x_d, img_sobel_y_d, width, height, low_threshold, high_threshold, gaussian_kernel_d, FILTER_WIDTH, from_video);
		break;
	case CANNY_GUI:
	{
		int thresh_h = 100;
		int thresh_l = 50;
		cv::namedWindow("Output Image", cv::WINDOW_NORMAL);
		cv::createTrackbar("Threshold High", "Output Image", &thresh_h, 255);
		cv::createTrackbar("Threshold Low", "Output Image", &thresh_l, 255);
		while (true)
		{
			cannyMainKernelWrap((uchar4 *)img.data, img_d, img_sobel_x_d, img_sobel_y_d, width, height, thresh_l, thresh_h, gaussian_kernel_d, FILTER_WIDTH, from_video);
			cv::imshow("Output Image", img);
			if (cv::waitKey(1) == 27) // wait to press 'esc' key
			{
				break;
			}
		}
		break;
	}
	case OTSU_BIN:
		int threshold = otsuThreshold(img_gray_d, width, height);
		binarizeImgWrapper(img.data, img_gray_d, width, height, threshold);
		break;
	}
	auto end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
	// cout << "Execution time: " << duration.count() << "ms" << endl;

	// Showing the result
	if (mode != CANNY_GUI)
	{
		cv::Mat img_out;
		// Since otsu binarization is done on the grayscale image, we need to convert it to 8UC1(8 unsigned char 1 channel) before displaying
		if (mode == OTSU_BIN)
		{
			img_out = cv::Mat(height, width, CV_8UC1, img.data);
		}
		else
		{
			img_out = cv::Mat(height, width, CV_8UC4, img.data);
		}
		string window_name = "Output Image " + to_string(mode);
		// string filesave = "debug/" + to_string(mode) + "_cuda.jpg";
		cv::cvtColor(img_out, img_out, cv::COLOR_RGBA2BGR);
		cv::imshow(window_name, img_out);
		// cv::imwrite(filesave, img_out);

		// If not from video, wait for key press
		if (!from_video)
		{
			cv::waitKey(0);
		}
		cudaHostUnregister(img.data);
		img.release();
	}

	// (cuda)memory deallocations
	cudaFree(img_d);

	cudaFree(img_gray_d);
	cudaFree(img_blurred_d);
	cudaFree(gaussian_kernel_d);
	cudaFree(sobel_x_kernel_d);
	cudaFree(sobel_y_kernel_d);
	cudaFree(sobel_x_separable_d);
	cudaFree(sobel_x_separable_2_d);
	cudaFree(img_sobel_x_d);
	cudaFree(img_sobel_y_d);
	cudaFree(img_harris_d);
	// Error checking
	free(img_gray_h);
	free(img_harris_h);
	free(img_grayh2);
	free(gaussian_kernel);
	free(img_h);
	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess)
	{
		fprintf(stderr, "Error in main kernels: %s\n", cudaGetErrorString(err));
	}
}
/**
 * @brief Handles video processing: RGB to Gray, Gaussian Blur then Harris/ShiTomasi Corner Detection, Canny Edge Detection or Otsu binarization
 *
 * @param mode Execution mode. Can be HARRIS, SHI_TOMASI, CANNY, CANNY_MANUAL, CANNY_GUI, OTSU_BIN
 * @param filename Video filename
 * @param low_threshold Low threshold for Canny Edge Detection Manual mode
 * @param high_threshold  High threshold for Canny Edge Detection Manual mode
 */
void handleVideo(enum Mode mode, std::string filename, int low_threshold, int high_threshold)
{
	cv::VideoCapture cap(filename);
	if (!cap.isOpened())
	{
		std::cerr << "Error: Unable to load video." << std::endl;
		return;
	}

	int debug = 0;
	while (cap.isOpened())
	{
		int64 start_time = cv::getTickCount();
		cv::Mat img;
		cap >> img;
		if (img.empty())
		{
			break;
		}

		handleImage(mode, filename, low_threshold, high_threshold, true, img);
		// free(img.data);


		if (cv::waitKey(1)==27) // 27=esc key
		{
			break;
		}

		// debug++;
		// if(debug == 2){
		// 	break;
		// }
	}
}

/**
 * @brief This is just a simple naive demo to show a possible usage of harris corner detection, as such it is not optimized. \
 * Treshold and tollerance values in the mapCommonKernelWrap should be adjusted according to the video or images used. \
 *
 * @param filename Video filename
 * @param filename2 Video filename 2 in case of images
 * @param video  Flag to indicate if we are working with a video or images
 */

void opticalNaive(std::string filename, std::string filename2, bool video)
{

	cv::Mat prev_frame, next_frame;

	cv::VideoCapture cap(filename);

	if (video)
	{
		if (!cap.isOpened())
		{
			std::cerr << "Error: Unable to load video." << std::endl;
			return;
		}

		cap >> prev_frame;
		cap >> next_frame;
	}
	else
	{
		prev_frame = cv::imread(filename, cv::IMREAD_COLOR);
		next_frame = cv::imread(filename2, cv::IMREAD_COLOR);
	}


	cv::cvtColor(prev_frame, prev_frame, cv::COLOR_BGR2RGBA);
	cv::cvtColor(next_frame, next_frame, cv::COLOR_BGR2RGBA);

	bool first = true;

	// variable declarations
	int width = prev_frame.cols;
	int height = prev_frame.rows;
	int channels = prev_frame.channels();
	float treshold = 0.1;

	size_t img_size_h = width * height * channels * sizeof(unsigned char);
	size_t img_gray_size_h = width * height * sizeof(float);

	// device variable declarations
	uchar4 *img_d;
	uchar4 *img_d_2;
	float *img_gray_d;
	float *img_blurred_d;
	float *img_harris_d;
	float *img_sobel_x_d;
	float *img_sobel_x_d_2;
	float *img_sobel_y_d;
	float *img_sobel_y_d_2;
	float *img_gray_d_2;
	float *img_blurred_d_2;

	// mallocs
	unsigned char *img_h = (unsigned char *)malloc(img_size_h);

	// kernel devices
	float *sobel_x_kernel_d;
	float *sobel_y_kernel_d;
	float *gaussian_kernel = computeGaussianKernel(FILTER_WIDTH, FILTER_SIGMA);
	float *gaussian_kernel_d;
	float *harris_map1_d;
	float *harris_map2_d;
	int *idx1Mapping_d;
	int *idx2Mapping_d;

	// data to host
	cudaMalloc(&img_d, img_size_h);
	cudaMalloc(&img_d_2, img_size_h);
	cudaMalloc(&img_gray_d, img_gray_size_h);
	cudaMalloc(&img_gray_d_2, img_gray_size_h);
	cudaMalloc(&img_blurred_d, img_gray_size_h);
	cudaMalloc(&img_blurred_d_2, img_gray_size_h);
	cudaMalloc(&gaussian_kernel_d, FILTER_WIDTH * FILTER_WIDTH * sizeof(float));
	cudaMalloc(&sobel_x_kernel_d, 3 * 3 * sizeof(float));
	cudaMalloc(&sobel_y_kernel_d, 3 * 3 * sizeof(float));
	cudaMalloc(&img_sobel_x_d, img_gray_size_h);
	cudaMalloc(&img_sobel_x_d_2, img_gray_size_h);
	cudaMalloc(&img_sobel_y_d, img_gray_size_h);
	cudaMalloc(&img_sobel_y_d_2, img_gray_size_h);
	cudaMalloc(&img_harris_d, img_gray_size_h);
	cudaMalloc(&harris_map1_d, img_gray_size_h);
	cudaMalloc(&harris_map2_d, img_gray_size_h);
	cudaMalloc(&idx1Mapping_d, img_gray_size_h);
	cudaMalloc(&idx2Mapping_d, img_gray_size_h);

	cudaMemcpy(img_d, prev_frame.data, img_size_h, cudaMemcpyHostToDevice);
	cudaMemcpy(img_d_2, next_frame.data, img_size_h, cudaMemcpyHostToDevice);
	cudaMemcpy(gaussian_kernel_d, gaussian_kernel, FILTER_WIDTH * FILTER_WIDTH * sizeof(float), cudaMemcpyHostToDevice);

	cudaMemcpy(sobel_x_kernel_d, sobel_x_kernel, 3 * 3 * sizeof(float), cudaMemcpyHostToDevice);
	cudaMemcpy(sobel_y_kernel_d, sobel_y_kernel, 3 * 3 * sizeof(float), cudaMemcpyHostToDevice);

	int *idx1Mapping_h = (int *)malloc(width * height * sizeof(int));
	int *idx2Mapping_h = (int *)malloc(width * height * sizeof(int));
	int mappingCount = 0;

	while (true)
	{

		if (!first)
		{
			cap >> next_frame;
			if (next_frame.empty())
			{
				break;
			}
			
			cv::cvtColor(next_frame, next_frame, cv::COLOR_BGR2RGBA);
			cv::cvtColor(prev_frame, prev_frame, cv::COLOR_BGR2RGBA);

			cudaMemcpy(img_d_2, next_frame.data, img_size_h, cudaMemcpyHostToDevice);
		}

		cudaDeviceSynchronize();

		// RGB to Gray
		if (first)
			rgbToGrayKernelWrap(img_d, img_gray_d, width, height);
		rgbToGrayKernelWrap(img_d_2, img_gray_d_2, width, height);

		// //convert to host

		// Apply Gaussian Blur to grayscale image
		if (first)
			convolutionGPUWrap(img_blurred_d, img_gray_d, width, height, gaussian_kernel_d, 3);
		convolutionGPUWrap(img_blurred_d_2, img_gray_d_2, width, height, gaussian_kernel_d, 3);

		// Sobel X
		if (first)
			convolutionGPUWrap(img_sobel_x_d, img_blurred_d, width, height, sobel_x_kernel_d, 3);
		convolutionGPUWrap(img_sobel_x_d_2, img_blurred_d_2, width, height, sobel_x_kernel_d, 3);

		// Sobel Y
		if (first)
			convolutionGPUWrap(img_sobel_y_d, img_blurred_d, width, height, sobel_y_kernel_d, 3);
		convolutionGPUWrap(img_sobel_y_d_2, img_blurred_d_2, width, height, sobel_y_kernel_d, 3);

		if (first)
			harrisMainKernelWrap((uchar4 *)prev_frame.data, img_d, img_sobel_x_d, img_sobel_y_d, width, height, K, ALPHA, gaussian_kernel_d, FILTER_WIDTH, false, harris_map1_d);
		treshold = harrisMainKernelWrap((uchar4 *)next_frame.data, img_d_2, img_sobel_x_d_2, img_sobel_y_d_2, width, height, K, ALPHA, gaussian_kernel_d, FILTER_WIDTH, false, harris_map2_d);

		//TOLLERANCE, WINDOW
		// 0.001,200 for 1-opt and 2-opt
		// 0.1,5 for cars
		// 0.5,5 for arrows
	
		mappingCount = mapCommonKernelWrap(harris_map1_d, harris_map2_d, width, height, treshold, 0.5, 5, idx1Mapping_d, idx2Mapping_d);

		cudaMemcpy(idx1Mapping_h, idx1Mapping_d, width * height * sizeof(int), cudaMemcpyDeviceToHost);
		cudaMemcpy(idx2Mapping_h, idx2Mapping_d, width * height * sizeof(int), cudaMemcpyDeviceToHost);

		cv::Point2f sumVec(0, 0);
		int validCount = 0;

		// draw motion vectors: on CPU because small amount of data + this is just for visualization purposes (and the implementation of our optical flow is just a naive demonstration)
		for (int i = 0; i < mappingCount; i++)
		{
			cv::Point pt1 = cv::Point(idx1Mapping_h[i] % next_frame.cols, idx1Mapping_h[i] / next_frame.cols);
			cv::Point pt2 = cv::Point(idx2Mapping_h[i] % next_frame.cols, idx2Mapping_h[i] / next_frame.cols);

			if ((pt1.x != pt2.x || pt1.y != pt2.y) &&
				pt1.x >= 0 && pt1.y >= 0 && pt1.x < prev_frame.cols && pt1.y < prev_frame.rows &&
				pt2.x >= 0 && pt2.y >= 0 && pt2.x < next_frame.cols && pt2.y < next_frame.rows)
			{

				cv::Scalar lightGreen(0, 0, 255);
				double tipLength = 0.08;

				cv::arrowedLine(next_frame, pt1, pt2, lightGreen, 1.5, cv::LINE_AA, 0, tipLength);

				// accumulate motion vector
				sumVec += Point2f(pt2 - pt1);
				validCount++;
			}
		}
		// average motion vector
		if (validCount > 0)
		{
			sumVec.x /= validCount;
			sumVec.y /= validCount;

			cv::Point center(next_frame.cols / 2, next_frame.rows / 2);
			cv::Point avgEnd = center + cv::Point(sumVec.x * 20, sumVec.y * 20); //arrow scale factor

			// clamp to image size
			avgEnd.x = std::max(0, std::min(avgEnd.x, next_frame.cols - 1));
			avgEnd.y = std::max(0, std::min(avgEnd.y, next_frame.rows - 1));
			cv::Scalar red(255, 0, 0);
			cv::arrowedLine(next_frame, center, avgEnd, red, 2, cv::LINE_AA, 0, 0.5);
		}

		// BACK TO RGB
		cv::cvtColor(next_frame, next_frame, cv::COLOR_RGBA2BGR);
		cv::imshow("Frame", next_frame);

		if (cv::waitKey(1) == 27) // 27=esc key
		{
			break;
		}

		if (!video)
		{
			cv::waitKey(0);
			break;
		}

		// std::this_thread::sleep_for(std::chrono::milliseconds(20));

		first = false;
		prev_frame = next_frame.clone();
		cudaMemcpy(img_d, img_d_2, img_size_h, cudaMemcpyDeviceToDevice);
		cudaMemcpy(img_gray_d, img_gray_d_2, width * height * sizeof(float), cudaMemcpyDeviceToDevice);
		cudaMemcpy(img_blurred_d, img_blurred_d_2, width * height * sizeof(float), cudaMemcpyDeviceToDevice);
		cudaMemcpy(img_sobel_x_d, img_sobel_x_d_2, width * height * sizeof(float), cudaMemcpyDeviceToDevice);
		cudaMemcpy(img_sobel_y_d, img_sobel_y_d_2, width * height * sizeof(float), cudaMemcpyDeviceToDevice);
		cudaMemcpy(harris_map1_d, harris_map2_d, width * height * sizeof(float), cudaMemcpyDeviceToDevice);
		cudaMemcpy(idx1Mapping_d, idx2Mapping_d, width * height * sizeof(int), cudaMemcpyDeviceToDevice);
		cudaDeviceSynchronize();
	}

	// (cuda)memory deallocations
	cudaFree(img_d);
	cudaFree(img_d_2);
	cudaFree(img_gray_d);
	cudaFree(img_gray_d_2);
	cudaFree(img_blurred_d);
	cudaFree(img_blurred_d_2);
	cudaFree(gaussian_kernel_d);
	cudaFree(sobel_x_kernel_d);
	cudaFree(sobel_y_kernel_d);
	cudaFree(img_sobel_x_d);
	cudaFree(img_sobel_x_d_2);
	cudaFree(img_sobel_y_d);
	cudaFree(img_sobel_y_d_2);
	cudaFree(img_harris_d);

	free(img_h);
	// Error checking
	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess)
	{
		fprintf(stderr, "Error in main kernels: %s\n", cudaGetErrorString(err));
	}
}

int main(const int argc, const char **argv)
{
	enum Mode mode;
	bool is_video = false;
#pragma region Arguments Parsing
	if (argc < 3)
	{
		fprintf(stderr, "Not enough arguments, at least 3 are required. Usage: %s [-H | -C | -O | -S | -OP] -f=filename\n", argv[0]);
		return -1;
	}
	if (strcmp(argv[1], "-H") == 0)
	{
		mode = HARRIS;
	}
	else if (strcmp(argv[1], "-C") == 0)
	{
		mode = CANNY;
	}
	else if (strcmp(argv[1], "-O") == 0)
	{
		mode = OTSU_BIN;
	}
	else if (strcmp(argv[1], "-S") == 0)
	{
		mode = SHI_TOMASI;
	}
	else if (strcmp(argv[1], "-OP") == 0)
	{
		mode = OPTICAL;
	}
	else
	{
		fprintf(stderr, "No execution mode specified. Usage: %s [-H | -C | -O | -S | -OP] -f=filename\n", argv[0]);
		return -1;
	}

	std::string filename = "";
	std::string filename2 = "";
	std::string arg = argv[2];
	if (arg.substr(0, 3) == "-f=")
	{
		filename = arg.substr(3);
		if (filename == "")
		{
			fprintf(stderr, "Empty filename. Usage: %s [-H | -C | -O | -S | -OP] -f=filename\n", argv[0]);
			return -1;
		}

		std::string ext = filename.substr(filename.find_last_of(".") + 1);
		if (ext != "jpg" && ext != "png" && ext != "mp4")
		{
			fprintf(stderr, "Invalid file extension. Only jpg, png and mp4 are supported. Usage: %s [-H | -C | -O | -S | -OP] -f=filename\n", argv[0]);
			return -1;
		}
		if (ext == "mp4")
		{
			is_video = true;
		}
	}
	else
	{
		fprintf(stderr, "No file specified. Usage: %s [-H | -C | -O | -S | -OP] -f=filename\n", argv[0]);
		return -1;
	}

	int low_threshold = 0;
	int high_threshold = 0;
	if (argc > 3)
	{
		if (mode == CANNY)
		{
			std::string arg = argv[3];
			if (arg == "-g")
			{
				if (is_video)
				{
					fprintf(stderr, "Cannot use GUI thresholding with videos. Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
					return -1;
				}
				mode = CANNY_GUI;
			}
			else
			{
				if (argc < 5)
				{
					fprintf(stderr, "No -G specified, implying manual thresholding. Not enough parameters. -l and -h required. Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
					return -1;
				}
				if (arg.substr(0, 3) == "-l=")
				{
					if (arg.substr(3) == "")
					{
						fprintf(stderr, "Invalid low threshold. Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
						return -1;
					}
					try
					{
						low_threshold = std::stoi(arg.substr(3));
					}
					catch (const std::exception &e)
					{
						fprintf(stderr, "Invalid low threshold. Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
						return -1;
					}
					mode = CANNY_MANUAL;
				}
				else
				{
					fprintf(stderr, "You need to specify a low threshold with \"-l=<int>\". Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
					return -1;
				}
				arg = argv[4];
				if (arg.substr(0, 3) == "-h=")
				{
					if (arg.substr(3) == "")
					{
						fprintf(stderr, "Invalid high threshold. Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
						return -1;
					}
					try
					{
						high_threshold = std::stoi(arg.substr(3));
					}
					catch (const std::exception &e)
					{
						fprintf(stderr, "Invalid high threshold. Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
						return -1;
					}
				}
				else
				{
					fprintf(stderr, "You need to specify a high threshold with \"-l=<int>\". Usage: %s -C -f=filename [-G | [-l=low_threshold -h=high_threshold]\n", argv[0]);
					return -1;
				}
				if (argc > 5)
				{
					fprintf(stderr, "Too many arguments for the specified mode. Ignoring extra arguments.\n");
				}
			}
		}
		else if (mode == OPTICAL)
		{
			std::string arg = argv[3];
			if (arg.substr(0, 4) == "-f2=")
			{
				filename2 = arg.substr(4);
				if (filename == "")
				{
					fprintf(stderr, "Empty filename. Usage for 2 images: %s -OP -f=filename -f2=filename2\n", argv[0]);
					return -1;
				}

				std::string ext = filename.substr(filename.find_last_of(".") + 1);
				if (ext != "jpg" && ext != "png" && ext != "mp4")
				{
					fprintf(stderr, "Invalid file extension. Only jpg, png and mp4 are supported. Usage for 2 images: %s -OP -f=filename -f2=filename2\n", argv[0]);
					return -1;
				}
				if (ext == "mp4")
				{
					is_video = true;
				}
			}
			else
			{
				fprintf(stderr, "No file specified. Usage for 2 images: %s -OP -f=filename -f2=filename2\n", argv[0]);
				return -1;
			}
		}
	}
#pragma endregion
#pragma region Driver Code
	if (mode == OPTICAL)
	{
		opticalNaive(filename, filename2, is_video);
	}
	else
	{
		if (is_video)
		{
			handleVideo(mode, filename, low_threshold, high_threshold);
		}
		else
		{
			handleImage(mode, filename, low_threshold, high_threshold);
		}
	}
#pragma endregion
	return 0;
}
// g++ -std=c++11 -IC:C:\opencv\opencv\build\include  -LC:C:\opencv\opencv\build\x64\vc15\lib -lopencv_core470 -lopencv_highgui470 -lopencv_imgcodecs470 -lopencv_imgproc470 -o my_program.exe main.cpp