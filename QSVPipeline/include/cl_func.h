﻿//  -----------------------------------------------------------------------------------------
//    QSVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  -----------------------------------------------------------------------------------------

#ifndef _CL_FUNC_H_
#define _CL_FUNC_H_

#include "qsv_version.h"

#if ENABLE_OPENCL

#include <Windows.h>
#include <CL/cl.h>

typedef cl_int (CL_API_CALL* funcClGetPlatformIDs)(cl_uint num_entries, cl_platform_id *platforms, cl_uint *num_platforms);
typedef cl_int (CL_API_CALL* funcClGetPlatformInfo) (cl_platform_id platform, cl_platform_info param_name, size_t param_value_size, void *param_value, size_t *param_value_size_ret);
typedef cl_int (CL_API_CALL* funcClGetDeviceIDs) (cl_platform_id platform, cl_device_type device_type, cl_uint num_entries, cl_device_id *devices, cl_uint *num_devices);
typedef cl_int (CL_API_CALL* funcClGetDeviceInfo) (cl_device_id device, cl_device_info param_name, size_t param_value_size, void *param_value, size_t *param_value_size_ret);

typedef cl_context (CL_API_CALL* funcClCreateContext) (const cl_context_properties * properties, cl_uint num_devices, const cl_device_id * devices, void (CL_CALLBACK * pfn_notify)(const char *, const void *, size_t, void *), void * user_data, cl_int * errcode_ret);
typedef cl_int (CL_API_CALL* funcClReleaseContext) (cl_context context);
typedef cl_command_queue (CL_API_CALL* funcClCreateCommandQueue)(cl_context context, cl_device_id device, cl_command_queue_properties properties, cl_int * errcode_ret);
typedef cl_int (CL_API_CALL* funcClReleaseCommandQueue) (cl_command_queue command_queue);

typedef cl_program(CL_API_CALL* funcClCreateProgramWithSource) (cl_context context, cl_uint count, const char **strings, const size_t *lengths, cl_int *errcode_ret);
typedef cl_int (CL_API_CALL* funcClBuildProgram) (cl_program program, cl_uint num_devices, const cl_device_id *device_list, const char *options, void (CL_CALLBACK *pfn_notify)(cl_program program, void *user_data), void* user_data);
typedef cl_int (CL_API_CALL* funcClGetProgramBuildInfo) (cl_program program, cl_device_id device, cl_program_build_info param_name, size_t param_value_size, void *param_value, size_t *param_value_size_ret);
typedef cl_int (CL_API_CALL* funcClReleaseProgram) (cl_program program);

typedef cl_mem (CL_API_CALL* funcClCreateBuffer) (cl_context context, cl_mem_flags flags, size_t size, void *host_ptr, cl_int *errcode_ret);
typedef cl_int (CL_API_CALL* funcClReleaseMemObject) (cl_mem memobj);
typedef cl_kernel (CL_API_CALL* funcClCreateKernel) (cl_program program, const char *kernel_name, cl_int *errcode_ret);
typedef cl_int (CL_API_CALL* funcClReleaseKernel) (cl_kernel kernel);
typedef cl_int (CL_API_CALL* funcClSetKernelArg) (cl_kernel kernel, cl_uint arg_index, size_t arg_size, const void *arg_value);
typedef cl_int (CL_API_CALL* funcClEnqueueNDRangeKernel)(cl_command_queue command_queue, cl_kernel kernel, cl_uint work_dim, const size_t *global_work_offset, const size_t *global_work_size, const size_t *local_work_size, cl_uint num_events_in_wait_list, const cl_event *event_wait_list, cl_event * event);
typedef cl_int(CL_API_CALL* funcClEnqueueTask) (cl_command_queue command_queue, cl_kernel kernel, cl_uint num_events_in_wait_list, const cl_event *event_wait_list, cl_event *event);

typedef cl_int(CL_API_CALL* funcClFinish) (cl_command_queue command_queue);
typedef cl_int(CL_API_CALL* funcClEnqueueReadBuffer) (cl_command_queue command_queue, cl_mem buffer, cl_bool blocking_read, size_t offset, size_t size, void *ptr, cl_uint num_events_in_wait_list, const cl_event *event_wait_list, cl_event *event);
typedef cl_int(CL_API_CALL* funcClEnqueueWriteBuffer) (cl_command_queue command_queue, cl_mem buffer, cl_bool blocking_write, size_t offset, size_t size, const void *ptr, cl_uint num_events_in_wait_list, const cl_event *event_wait_list, cl_event *event);

typedef struct cl_func_t {
	HMODULE hdll;

	funcClGetPlatformIDs getPlatformIDs;
	funcClGetPlatformInfo getPlatformInfo;
	funcClGetDeviceIDs getDeviceIDs;
	funcClGetDeviceInfo getDeviceInfo;

	funcClCreateProgramWithSource createProgramWithSource;
	funcClBuildProgram buildProgram;
	funcClGetProgramBuildInfo getProgramBuildInfo;
	funcClReleaseProgram releaseProgram;

	funcClCreateContext createContext;
	funcClReleaseContext releaseContext;
	funcClCreateCommandQueue createCommandQueue;
	funcClReleaseCommandQueue releaseCommandQueue;
	funcClCreateBuffer createBuffer;
	funcClReleaseMemObject releaseMemObject;

	funcClCreateKernel createKernel;
	funcClReleaseKernel releaseKernel;
	funcClSetKernelArg setKernelArg;
	funcClEnqueueTask enqueueTask;
	funcClEnqueueNDRangeKernel enqueueNDRangeKernel;

	funcClFinish finish;
	funcClEnqueueReadBuffer enqueueReadBuffer;
	funcClEnqueueWriteBuffer enqueueWriteBuffer;
} cl_func_t;

typedef struct cl_data_t {
	cl_platform_id platformID;
	cl_device_id deviceID;
	cl_context contextCL;
	cl_program program;
	cl_command_queue commands;
	cl_kernel kernel;
} cl_data_t;

cl_int cl_get_func(cl_func_t *cl);
void cl_release_func(cl_func_t *cl);

cl_int cl_get_platform_and_device(const char *VendorName, cl_data_t *cl_data, const cl_func_t *cl);

void cl_release(cl_data_t *cl_data, cl_func_t *cl);

#endif //ENABLE_OPENCL

#endif //_CL_FUNC_H_
