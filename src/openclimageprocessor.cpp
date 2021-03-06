#include <cassert>
#include <cmath>
#include <regex>
#include <stdexcept>
#include "imageprocessor.h"
#include "openclimageprocessor.h"
#include "cl.hpp"

using namespace std;

/***  Private Methods  ********************************************************/
cl::Kernel OpenclImageProcessor::loadKernel(string filename, string kernel_name)
{
    ifstream cl_file("kernels/" + filename);
    if (!cl_file.good())
        cerr << "Couldn't open " << filename<< endl;
    string cl_string(istreambuf_iterator<char>(cl_file),
            (istreambuf_iterator<char>()));
    cl::Program::Sources source(1, make_pair(cl_string.c_str(),
                cl_string.length() + 1));
    cl::Program program(context, source);
    try
    {
        program.build(devices);
    }
    catch (cl::Error e)
    {
        // If there's a build error, print out the build log to see what
        // exactly the problem was.
        cerr << "Build Status:\t"
             << program.getBuildInfo<CL_PROGRAM_BUILD_STATUS>(selectedDevice)
             << endl
             << "Build Options:\t"
             << program.getBuildInfo<CL_PROGRAM_BUILD_OPTIONS>(selectedDevice)
             << endl
             << "Build Log:\t "
             << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(selectedDevice)
             << endl;
    }

    return cl::Kernel(program, kernel_name.c_str());
}

// Returns (hopefully) the discrete GPU in devices. If none are found, then the
// first GPU is returned.
cl::Device &OpenclImageProcessor::findBestDevice()
{
    if (devices.size() == 0)
        throw std::out_of_range("No devices in devices vector.");
    
    // look for nvidia, amd, or ati. This may yield a false positive for
    // integrated amd GPUs, but it's better than the current solution.
    std::regex valid_device("(NVIDIA|AMD|ATI)", std::regex_constants::icase);
    for (auto &d : devices)
    {
        if (std::regex_search(d.getInfo<CL_DEVICE_VENDOR>(), valid_device))
            return d;
    }

    return devices[0];
}

/***  Public Methods **********************************************************/

OpenclImageProcessor::OpenclImageProcessor(bool UseGPU)
{
    try
    {
        // OpenCL Initialization
        cl::Platform::get(&platforms);

        if (UseGPU)
            platforms[0].getDevices(CL_DEVICE_TYPE_GPU, &devices);
        else
            platforms[0].getDevices(CL_DEVICE_TYPE_CPU, &devices);

        selectedDevice = findBestDevice();
        context = cl::Context(devices);
        queue = cl::CommandQueue(context, selectedDevice);
        
        // create and load the kernels
        gaussian = loadKernel("gaussian_kernel.cl", "gaussian_kernel");

        // currently using the edge kernel as the sobel kernel
        sobel = loadKernel("sobel_kernel.cl", "sobel_kernel");

        nonMaxSuppression = loadKernel("non_max_supp_kernel.cl",
                                       "non_max_supp_kernel");
        
        hysteresisThresholding = loadKernel("hyst_kernel.cl", "hyst_kernel");
    }
    catch (cl::Error e)
    {
        cerr << endl << "Error: " << e.what() << " : " << e.err() << endl;
    }
}

// outputs basic information about the device in use.
void OpenclImageProcessor::DeviceInfo()
{
    cout << "Device info:" << endl
         << "Name: " << selectedDevice.getInfo<CL_DEVICE_NAME>() << endl
         << "Vendor: " << selectedDevice.getInfo<CL_DEVICE_VENDOR>() << endl;

}

// load the 8bit 1channel grayscale Mat
void OpenclImageProcessor::LoadImage(cv::Mat &input)
{
    this->input = input;
    output = cv::Mat(input.rows, input.cols, CV_8UC1);
    nextBuff() = cl::Buffer(context,
                            CL_MEM_READ_WRITE |
                            CL_MEM_ALLOC_HOST_PTR | CL_MEM_COPY_HOST_PTR,
                            input.rows * input.cols * input.elemSize(),
                            input.data);
    prevBuff() = cl::Buffer(context,
                            CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                            input.rows * input.cols * input.elemSize());
                            
    // Initialize the theta buffer
    theta =  cl::Buffer(context,
                        CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                        input.rows * input.cols * input.elemSize());
    advanceBuff();
}

// copies the buffer back and returns it. this is a blocking call.
cv::Mat OpenclImageProcessor::GetOutput()
{
    // copy the buffer back
    queue.enqueueReadBuffer(prevBuff(), CL_TRUE, 0,
                            input.rows * input.cols * input.elemSize(),
                            output.data);

    queue.finish();
    assert(output.rows == input.rows && output.cols == input.cols);
    return output;
}

void OpenclImageProcessor::FinishJobs()
{
    queue.finish();
}

// enqueues the gaussian kernel
void OpenclImageProcessor::Gaussian()
{
    gaussian.setArg(0, prevBuff());
    gaussian.setArg(1, nextBuff());
    gaussian.setArg(2, input.rows);
    gaussian.setArg(3, input.cols);

    queue.enqueueNDRangeKernel(gaussian,
                               cl::NDRange(1, 1),
                               cl::NDRange(input.rows - 2,
                                           input.cols - 2),
                               cl::NDRange(1, 1),
                               NULL);

    advanceBuff();
}

// enqueues the sobel kernel
void OpenclImageProcessor::Sobel()
{
    sobel.setArg(0, prevBuff());
    sobel.setArg(1, nextBuff());
    sobel.setArg(2, theta);
    sobel.setArg(3, input.rows);
    sobel.setArg(4, input.cols);

    queue.enqueueNDRangeKernel(sobel,
                               cl::NDRange(1, 1),
                               cl::NDRange(input.rows - 2,
                                           input.cols - 2),
                               cl::NDRange(1, 1),
                               NULL);

    advanceBuff();
}

// enqueues the nonMaxSuppression kernel
void OpenclImageProcessor::NonMaxSuppression()
{
    nonMaxSuppression.setArg(0, prevBuff());
    nonMaxSuppression.setArg(1, nextBuff());
    nonMaxSuppression.setArg(2, theta);
    nonMaxSuppression.setArg(3, input.rows);
    nonMaxSuppression.setArg(4, input.cols);

    queue.enqueueNDRangeKernel(nonMaxSuppression,
            cl::NullRange,
            cl::NDRange(input.rows - 2, input.cols - 2),
            cl::NDRange(1, 1),
            NULL);

    advanceBuff();
}

// enqueues the hysteresisThresholding kernel
void OpenclImageProcessor::HysteresisThresholding()
{
    hysteresisThresholding.setArg(0, prevBuff());
    hysteresisThresholding.setArg(1, nextBuff());
    hysteresisThresholding.setArg(2, input.rows);
    hysteresisThresholding.setArg(3, input.cols);
    
    queue.enqueueNDRangeKernel(hysteresisThresholding,
                               cl::NullRange,
                               cl::NDRange(input.rows,
                                           input.cols),
                               cl::NDRange(1, 1),
                               NULL);
    
    advanceBuff();
}

// enqueues all of the canny stages
void OpenclImageProcessor::Canny()
{
    Gaussian();
    Sobel();
    NonMaxSuppression();
    HysteresisThresholding();
}
