/*
 * Copyright (c) 2018 Sergey Lavrushkin
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * DNN tensorflow backend implementation.
 */

#include "dnn_backend_tf.h"
#include "dnn_backend_native.h"
#include "dnn_backend_native_layer_conv2d.h"
#include "dnn_backend_native_layer_depth2space.h"
#include "libavformat/avio.h"
#include "libavutil/avassert.h"
#include "dnn_backend_native_layer_pad.h"
#include "dnn_backend_native_layer_maximum.h"
#include "compat/dnn/libtf_wrapper.h"

typedef struct TFModel{
    TF_Graph *graph;
    TF_Session *session;
    TF_Status *status;
    TF_Output input;
    TF_Tensor *input_tensor;
    TF_Output *outputs;
    TF_Tensor **output_tensors;
    uint32_t nb_output;
    void* libtensorflow;
    TFFunctions* tffns;
} TFModel;

//CUDA device ID to support multi GPU
int32_t deviceid = -1;

static void free_buffer(void *data, size_t length)
{
    av_freep(&data);
}

static TF_Buffer *read_graph(TFModel *tf_model, const char *model_filename)
{
    TF_Buffer *graph_buf;
    unsigned char *graph_data = NULL;
    AVIOContext *model_file_context;
    long size, bytes_read;

    if (avio_open(&model_file_context, model_filename, AVIO_FLAG_READ) < 0){
        return NULL;
    }

    size = avio_size(model_file_context);

    graph_data = av_malloc(size);
    if (!graph_data){
        avio_closep(&model_file_context);
        return NULL;
    }
    bytes_read = avio_read(model_file_context, graph_data, size);
    avio_closep(&model_file_context);
    if (bytes_read != size){
        av_freep(&graph_data);
        return NULL;
    }

    graph_buf = tf_model->tffns->TF_NewBuffer();
    graph_buf->data = (void *)graph_data;
    graph_buf->length = size;
    graph_buf->data_deallocator = free_buffer;

    return graph_buf;
}

static TF_Tensor *allocate_input_tensor(TFModel *tf_model, const DNNData *input)
{
    TF_DataType dt;
    size_t size;
    int64_t input_dims[] = {1, input->height, input->width, input->channels};
    switch (input->dt) {
    case DNN_FLOAT:
        dt = TF_FLOAT;
        size = sizeof(float);
        break;
    case DNN_UINT8:
        dt = TF_UINT8;
        size = sizeof(char);
        break;
    default:
        av_assert0(!"should not reach here");
    }

    return tf_model->tffns->TF_AllocateTensor(dt, input_dims, 4,
                             input_dims[1] * input_dims[2] * input_dims[3] * size);
}

static DNNReturnType get_input_tf(void *model, DNNData *input, const char *input_name)
{
    TFModel *tf_model = (TFModel *)model;
    TF_Status *status;
    int64_t dims[4];

    TF_Output tf_output;
    tf_output.oper = tf_model->tffns->TF_GraphOperationByName(tf_model->graph, input_name);
    if (!tf_output.oper)
        return DNN_ERROR;

    tf_output.index = 0;
    input->dt = tf_model->tffns->TF_OperationOutputType(tf_output);

    status = tf_model->tffns->TF_NewStatus();
    tf_model->tffns->TF_GraphGetTensorShape(tf_model->graph, tf_output, dims, 4, status);
    if (tf_model->tffns->TF_GetCode(status) != TF_OK){
        tf_model->tffns->TF_DeleteStatus(status);
        return DNN_ERROR;
    }
    tf_model->tffns->TF_DeleteStatus(status);

    //currently only NHWC is supported
    av_assert0(dims[0] == 1 || dims[0] == -1);
    input->height = dims[1];
    input->width = dims[2];
    input->channels = dims[3];

    return DNN_SUCCESS;
}

static DNNReturnType set_input_output_tf(void *model, DNNData *input, const char *input_name, const char **output_names, uint32_t nb_output)
{
    TFModel *tf_model = (TFModel *)model;
    TF_SessionOptions *sess_opts;
    const TF_Operation* init_op = tf_model->tffns->TF_GraphOperationByName(tf_model->graph, "init");

    // Input operation
    tf_model->input.oper = tf_model->tffns->TF_GraphOperationByName(tf_model->graph, input_name);
    if (!tf_model->input.oper){
        return DNN_ERROR;
    }
    tf_model->input.index = 0;
    if (tf_model->input_tensor){
        tf_model->tffns->TF_DeleteTensor(tf_model->input_tensor);
    }
    tf_model->input_tensor = allocate_input_tensor(tf_model, input);
    if (!tf_model->input_tensor){
        return DNN_ERROR;
    }
    input->data = (float *)tf_model->tffns->TF_TensorData(tf_model->input_tensor);

    // Output operation
    if (nb_output == 0)
        return DNN_ERROR;

    av_freep(&tf_model->outputs);
    tf_model->outputs = av_malloc_array(nb_output, sizeof(*tf_model->outputs));
    if (!tf_model->outputs)
        return DNN_ERROR;
    for (int i = 0; i < nb_output; ++i) {
        tf_model->outputs[i].oper = tf_model->tffns->TF_GraphOperationByName(tf_model->graph, output_names[i]);
        if (!tf_model->outputs[i].oper){
            av_freep(&tf_model->outputs);
            return DNN_ERROR;
        }
        tf_model->outputs[i].index = 0;
    }

    if (tf_model->output_tensors) {
        for (uint32_t i = 0; i < tf_model->nb_output; ++i) {
            if (tf_model->output_tensors[i]) {
                tf_model->tffns->TF_DeleteTensor(tf_model->output_tensors[i]);
                tf_model->output_tensors[i] = NULL;
            }
        }
    }
    av_freep(&tf_model->output_tensors);
    tf_model->output_tensors = av_mallocz_array(nb_output, sizeof(*tf_model->output_tensors));
    if (!tf_model->output_tensors) {
        av_freep(&tf_model->outputs);
        return DNN_ERROR;
    }

    tf_model->nb_output = nb_output;

    if (tf_model->session){
        tf_model->tffns->TF_CloseSession(tf_model->session, tf_model->status);
        tf_model->tffns->TF_DeleteSession(tf_model->session, tf_model->status);
    }

    sess_opts = tf_model->tffns->TF_NewSessionOptions();
    // protobuf data for auto memory gpu_options.allow_growth=True
    uint8_t config[4] = { 0x32, 0x02, 0x20, 0x1 };
    tf_model->tffns->TF_SetConfig(sess_opts, (void*)config, 4, tf_model->status);

    tf_model->session = tf_model->tffns->TF_NewSession(tf_model->graph, sess_opts, tf_model->status);
    tf_model->tffns->TF_DeleteSessionOptions(sess_opts);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK)
    {
        return DNN_ERROR;
    }

    // Run initialization operation with name "init" if it is present in graph
    if (init_op){
        tf_model->tffns->TF_SessionRun(tf_model->session, NULL,
                      NULL, NULL, 0,
                      NULL, NULL, 0,
                      &init_op, 1, NULL, tf_model->status);
        if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK)
        {
            return DNN_ERROR;
        }
    }

    return DNN_SUCCESS;
}

static DNNReturnType load_tf_model(TFModel *tf_model, const char *model_filename)
{
    TF_Buffer *graph_def;
    TF_ImportGraphDefOptions *graph_opts;
    char sdevice[64] = {0,};

    graph_def = read_graph(tf_model, model_filename);
    if (!graph_def){
        return DNN_ERROR;
    }
    tf_model->graph = tf_model->tffns->TF_NewGraph();
    tf_model->status = tf_model->tffns->TF_NewStatus();
    graph_opts = tf_model->tffns->TF_NewImportGraphDefOptions();
    if(deviceid >= 0) {
        sprintf(sdevice,"/gpu:%d", deviceid);
        tf_model->tffns->TF_ImportGraphDefOptionsSetDefaultDevice(graph_opts, sdevice);
        //restore default value
        deviceid = -1;
	}
    tf_model->tffns->TF_GraphImportGraphDef(tf_model->graph, graph_def, graph_opts, tf_model->status);
    tf_model->tffns->TF_DeleteImportGraphDefOptions(graph_opts);
    tf_model->tffns->TF_DeleteBuffer(graph_def);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        tf_model->tffns->TF_DeleteGraph(tf_model->graph);
        tf_model->tffns->TF_DeleteStatus(tf_model->status);
        return DNN_ERROR;
   }

    return DNN_SUCCESS;
}

#define NAME_BUFFER_SIZE 256

static DNNReturnType add_conv_layer(TFModel *tf_model, TF_Operation *transpose_op, TF_Operation **cur_op,
                                    ConvolutionalParams* params, const int layer)
{
    TF_Operation *op;
    TF_OperationDescription *op_desc;
    TF_Output input;
    int64_t strides[] = {1, 1, 1, 1};
    TF_Tensor *tensor;
    int64_t dims[4];
    int dims_len;
    char name_buffer[NAME_BUFFER_SIZE];
    int32_t size;

    size = params->input_num * params->output_num * params->kernel_size * params->kernel_size;
    input.index = 0;

    snprintf(name_buffer, NAME_BUFFER_SIZE, "conv_kernel%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Const", name_buffer);
    tf_model->tffns->TF_SetAttrType(op_desc, "dtype", TF_FLOAT);
    dims[0] = params->output_num;
    dims[1] = params->kernel_size;
    dims[2] = params->kernel_size;
    dims[3] = params->input_num;
    dims_len = 4;
    tensor = tf_model->tffns->TF_AllocateTensor(TF_FLOAT, dims, dims_len, size * sizeof(float));
    memcpy(tf_model->tffns->TF_TensorData(tensor), params->kernel, size * sizeof(float));
    tf_model->tffns->TF_SetAttrTensor(op_desc, "value", tensor, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }
    op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    snprintf(name_buffer, NAME_BUFFER_SIZE, "transpose%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Transpose", name_buffer);
    input.oper = op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    input.oper = transpose_op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    tf_model->tffns->TF_SetAttrType(op_desc, "Tperm", TF_INT32);
    op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    snprintf(name_buffer, NAME_BUFFER_SIZE, "conv2d%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Conv2D", name_buffer);
    input.oper = *cur_op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    input.oper = op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    tf_model->tffns->TF_SetAttrIntList(op_desc, "strides", strides, 4);
    tf_model->tffns->TF_SetAttrString(op_desc, "padding", "VALID", 5);
    *cur_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    snprintf(name_buffer, NAME_BUFFER_SIZE, "conv_biases%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Const", name_buffer);
    tf_model->tffns->TF_SetAttrType(op_desc, "dtype", TF_FLOAT);
    dims[0] = params->output_num;
    dims_len = 1;
    tensor = tf_model->tffns->TF_AllocateTensor(TF_FLOAT, dims, dims_len, params->output_num * sizeof(float));
    memcpy(tf_model->tffns->TF_TensorData(tensor), params->biases, params->output_num * sizeof(float));
    tf_model->tffns->TF_SetAttrTensor(op_desc, "value", tensor, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }
    op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    snprintf(name_buffer, NAME_BUFFER_SIZE, "bias_add%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "BiasAdd", name_buffer);
    input.oper = *cur_op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    input.oper = op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    *cur_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    snprintf(name_buffer, NAME_BUFFER_SIZE, "activation%d", layer);
    switch (params->activation){
    case RELU:
        op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Relu", name_buffer);
        break;
    case TANH:
        op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Tanh", name_buffer);
        break;
    case SIGMOID:
        op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Sigmoid", name_buffer);
        break;
    default:
        return DNN_ERROR;
    }
    input.oper = *cur_op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    *cur_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    return DNN_SUCCESS;
}

static DNNReturnType add_depth_to_space_layer(TFModel *tf_model, TF_Operation **cur_op,
                                              DepthToSpaceParams *params, const int layer)
{
    TF_OperationDescription *op_desc;
    TF_Output input;
    char name_buffer[NAME_BUFFER_SIZE];

    snprintf(name_buffer, NAME_BUFFER_SIZE, "depth_to_space%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "DepthToSpace", name_buffer);
    input.oper = *cur_op;
    input.index = 0;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    tf_model->tffns->TF_SetAttrInt(op_desc, "block_size", params->block_size);
    *cur_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    return DNN_SUCCESS;
}

static DNNReturnType add_pad_layer(TFModel *tf_model, TF_Operation **cur_op,
                                              LayerPadParams *params, const int layer)
{
    TF_Operation *op;
    TF_Tensor *tensor;
    TF_OperationDescription *op_desc;
    TF_Output input;
    int32_t *pads;
    int64_t pads_shape[] = {4, 2};

    char name_buffer[NAME_BUFFER_SIZE];
    snprintf(name_buffer, NAME_BUFFER_SIZE, "pad%d", layer);

    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Const", name_buffer);
    tf_model->tffns->TF_SetAttrType(op_desc, "dtype", TF_INT32);
    tensor = tf_model->tffns->TF_AllocateTensor(TF_INT32, pads_shape, 2, 4 * 2 * sizeof(int32_t));
    pads = (int32_t *)tf_model->tffns->TF_TensorData(tensor);
    pads[0] = params->paddings[0][0];
    pads[1] = params->paddings[0][1];
    pads[2] = params->paddings[1][0];
    pads[3] = params->paddings[1][1];
    pads[4] = params->paddings[2][0];
    pads[5] = params->paddings[2][1];
    pads[6] = params->paddings[3][0];
    pads[7] = params->paddings[3][1];
    tf_model->tffns->TF_SetAttrTensor(op_desc, "value", tensor, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }
    op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "MirrorPad", "mirror_pad");
    input.oper = *cur_op;
    input.index = 0;
    tf_model->tffns->TF_AddInput(op_desc, input);
    input.oper = op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    tf_model->tffns->TF_SetAttrType(op_desc, "Tpaddings", TF_INT32);
    tf_model->tffns->TF_SetAttrString(op_desc, "mode", "SYMMETRIC", 9);
    *cur_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    return DNN_SUCCESS;
}

static DNNReturnType add_maximum_layer(TFModel *tf_model, TF_Operation **cur_op,
                                       DnnLayerMaximumParams *params, const int layer)
{
    TF_Operation *op;
    TF_Tensor *tensor;
    TF_OperationDescription *op_desc;
    TF_Output input;
    float *y;

    char name_buffer[NAME_BUFFER_SIZE];
    snprintf(name_buffer, NAME_BUFFER_SIZE, "maximum/y%d", layer);

    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Const", name_buffer);
    tf_model->tffns->TF_SetAttrType(op_desc, "dtype", TF_FLOAT);
    tensor = tf_model->tffns->TF_AllocateTensor(TF_FLOAT, NULL, 0, tf_model->tffns->TF_DataTypeSize(TF_FLOAT));
    y = (float *)tf_model->tffns->TF_TensorData(tensor);
    *y = params->val.y;
    tf_model->tffns->TF_SetAttrTensor(op_desc, "value", tensor, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }
    op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    snprintf(name_buffer, NAME_BUFFER_SIZE, "maximum%d", layer);
    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Maximum", name_buffer);
    input.oper = *cur_op;
    input.index = 0;
    tf_model->tffns->TF_AddInput(op_desc, input);
    input.oper = op;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_SetAttrType(op_desc, "T", TF_FLOAT);
    *cur_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    return DNN_SUCCESS;
}

static DNNReturnType load_native_model(TFModel *tf_model, const char *model_filename)
{
    int32_t layer;
    TF_OperationDescription *op_desc;
    TF_Operation *op;
    TF_Operation *transpose_op;
    TF_Tensor *tensor;
    TF_Output input;
    int32_t *transpose_perm;
    int64_t transpose_perm_shape[] = {4};
    int64_t input_shape[] = {1, -1, -1, -1};
    DNNReturnType layer_add_res;
    DNNModel *native_model = NULL;
    ConvolutionalNetwork *conv_network;

    native_model = ff_dnn_load_model_native(model_filename);
    if (!native_model){
        return DNN_ERROR;
    }

    conv_network = (ConvolutionalNetwork *)native_model->model;
    tf_model->graph = tf_model->tffns->TF_NewGraph();
    tf_model->status = tf_model->tffns->TF_NewStatus();

#define CLEANUP_ON_ERROR(tf_model) \
    { \
        tf_model->tffns->TF_DeleteGraph(tf_model->graph); \
        tf_model->tffns->TF_DeleteStatus(tf_model->status); \
        return DNN_ERROR; \
    }

    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Placeholder", "x");
    tf_model->tffns->TF_SetAttrType(op_desc, "dtype", TF_FLOAT);
    tf_model->tffns->TF_SetAttrShape(op_desc, "shape", input_shape, 4);
    op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        CLEANUP_ON_ERROR(tf_model);
    }

    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Const", "transpose_perm");
    tf_model->tffns->TF_SetAttrType(op_desc, "dtype", TF_INT32);
    tensor = tf_model->tffns->TF_AllocateTensor(TF_INT32, transpose_perm_shape, 1, 4 * sizeof(int32_t));
    transpose_perm = (int32_t *)tf_model->tffns->TF_TensorData(tensor);
    transpose_perm[0] = 1;
    transpose_perm[1] = 2;
    transpose_perm[2] = 3;
    transpose_perm[3] = 0;
    tf_model->tffns->TF_SetAttrTensor(op_desc, "value", tensor, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        CLEANUP_ON_ERROR(tf_model);
    }
    transpose_op = tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);

    for (layer = 0; layer < conv_network->layers_num; ++layer){
        switch (conv_network->layers[layer].type){
        case DLT_INPUT:
            layer_add_res = DNN_SUCCESS;
            break;
        case DLT_CONV2D:
            layer_add_res = add_conv_layer(tf_model, transpose_op, &op,
                                           (ConvolutionalParams *)conv_network->layers[layer].params, layer);
            break;
        case DLT_DEPTH_TO_SPACE:
            layer_add_res = add_depth_to_space_layer(tf_model, &op,
                                                     (DepthToSpaceParams *)conv_network->layers[layer].params, layer);
            break;
        case DLT_MIRROR_PAD:
            layer_add_res = add_pad_layer(tf_model, &op,
                                          (LayerPadParams *)conv_network->layers[layer].params, layer);
            break;
        case DLT_MAXIMUM:
            layer_add_res = add_maximum_layer(tf_model, &op,
                                          (DnnLayerMaximumParams *)conv_network->layers[layer].params, layer);
            break;
        default:
            CLEANUP_ON_ERROR(tf_model);
        }

        if (layer_add_res != DNN_SUCCESS){
            CLEANUP_ON_ERROR(tf_model);
        }
    }

    op_desc = tf_model->tffns->TF_NewOperation(tf_model->graph, "Identity", "y");
    input.oper = op;
    input.index = 0;
    tf_model->tffns->TF_AddInput(op_desc, input);
    tf_model->tffns->TF_FinishOperation(op_desc, tf_model->status);
    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        CLEANUP_ON_ERROR(tf_model);
    }

    ff_dnn_free_model_native(&native_model);

    return DNN_SUCCESS;
}

static DNNReturnType load_libtensorflow(TFModel *tf_model)
{
    tf_model->libtensorflow = TF_LOAD_FUNC(TF_LIBNAME);
    if (!tf_model->libtensorflow)    {
        return DNN_ERROR;
    }

    tf_model->tffns = (TFFunctions*) av_malloc(sizeof(TFFunctions));

    TF_LOAD_SYMBOL(TF_Version)
    TF_LOAD_SYMBOL(TF_SessionRun)
    TF_LOAD_SYMBOL(TF_GetCode)
    TF_LOAD_SYMBOL(TF_Dim)
    TF_LOAD_SYMBOL(TF_TensorData)
    TF_LOAD_SYMBOL(TF_TensorType)
    TF_LOAD_SYMBOL(TF_CloseSession)
    TF_LOAD_SYMBOL(TF_DeleteSession)
    TF_LOAD_SYMBOL(TF_DeleteStatus)
    TF_LOAD_SYMBOL(TF_DeleteGraph)
    TF_LOAD_SYMBOL(TF_DeleteTensor)
    TF_LOAD_SYMBOL(TF_NewSession)
    TF_LOAD_SYMBOL(TF_NewGraph)
    TF_LOAD_SYMBOL(TF_GraphOperationByName)
    TF_LOAD_SYMBOL(TF_OperationOutputType)
    TF_LOAD_SYMBOL(TF_NewStatus)
    TF_LOAD_SYMBOL(TF_GraphGetTensorShape)
    TF_LOAD_SYMBOL(TF_GetCode)
    TF_LOAD_SYMBOL(TF_NewOperation)
    TF_LOAD_SYMBOL(TF_SetAttrType)
    TF_LOAD_SYMBOL(TF_SetAttrShape)
    TF_LOAD_SYMBOL(TF_FinishOperation)
    TF_LOAD_SYMBOL(TF_AllocateTensor)
    TF_LOAD_SYMBOL(TF_SetAttrTensor)
    TF_LOAD_SYMBOL(TF_AddInput)
    TF_LOAD_SYMBOL(TF_SetAttrInt)
    TF_LOAD_SYMBOL(TF_DataTypeSize)
    TF_LOAD_SYMBOL(TF_SetAttrString)
    TF_LOAD_SYMBOL(TF_SetAttrIntList)
    TF_LOAD_SYMBOL(TF_GraphOperationByName)
    TF_LOAD_SYMBOL(TF_NewSessionOptions)
    TF_LOAD_SYMBOL(TF_SetConfig)
    TF_LOAD_SYMBOL(TF_DeleteSessionOptions)
    TF_LOAD_SYMBOL(TF_DeleteSessionOptions)
    TF_LOAD_SYMBOL(TF_NewImportGraphDefOptions)
    TF_LOAD_SYMBOL(TF_ImportGraphDefOptionsSetDefaultDevice)
    TF_LOAD_SYMBOL(TF_GraphImportGraphDef)
    TF_LOAD_SYMBOL(TF_DeleteImportGraphDefOptions)
    TF_LOAD_SYMBOL(TF_DeleteBuffer)
    TF_LOAD_SYMBOL(TF_NewBuffer)


    return DNN_SUCCESS;
}

DNNModel *ff_dnn_load_model_tf(const char *model_filename)
{
    DNNModel *model = NULL;
    TFModel *tf_model = NULL;

    model = av_malloc(sizeof(DNNModel));
    if (!model){
        return NULL;
    }

    tf_model = av_mallocz(sizeof(TFModel));
    if (!tf_model){
        av_freep(&model);
        return NULL;
    }

    if (load_libtensorflow(tf_model) != DNN_SUCCESS) {
        return NULL;
    }

    if (load_tf_model(tf_model, model_filename) != DNN_SUCCESS){
        if (load_native_model(tf_model, model_filename) != DNN_SUCCESS){
            av_freep(&tf_model->tffns);
            TF_FREE_FUNC(tf_model->libtensorflow);
            av_freep(&tf_model);
            av_freep(&model);
            return NULL;
        }
    }

    model->model = (void *)tf_model;
    model->set_input_output = &set_input_output_tf;
    model->get_input = &get_input_tf;

    return model;
}

void ff_dnn_set_deviceid_tf(uint32_t gpuid)
{
    deviceid = gpuid;
}

DNNReturnType ff_dnn_execute_model_tf(const DNNModel *model, DNNData *outputs, uint32_t nb_output)
{
    TFModel *tf_model = (TFModel *)model->model;
    uint32_t nb = FFMIN(nb_output, tf_model->nb_output);
    if (nb == 0)
        return DNN_ERROR;

    av_assert0(tf_model->output_tensors);
    for (uint32_t i = 0; i < tf_model->nb_output; ++i) {
        if (tf_model->output_tensors[i]) {
            tf_model->tffns->TF_DeleteTensor(tf_model->output_tensors[i]);
            tf_model->output_tensors[i] = NULL;
        }
    }

    tf_model->tffns->TF_SessionRun(tf_model->session, NULL,
                  &tf_model->input, &tf_model->input_tensor, 1,
                  tf_model->outputs, tf_model->output_tensors, nb,
                  NULL, 0, NULL, tf_model->status);

    if (tf_model->tffns->TF_GetCode(tf_model->status) != TF_OK){
        return DNN_ERROR;
    }

    for (uint32_t i = 0; i < nb; ++i) {
        outputs[i].height = tf_model->tffns->TF_Dim(tf_model->output_tensors[i], 1);
        outputs[i].width = tf_model->tffns->TF_Dim(tf_model->output_tensors[i], 2);
        outputs[i].channels = tf_model->tffns->TF_Dim(tf_model->output_tensors[i], 3);
        outputs[i].data = tf_model->tffns->TF_TensorData(tf_model->output_tensors[i]);
        outputs[i].dt = tf_model->tffns->TF_TensorType(tf_model->output_tensors[i]);
    }

    return DNN_SUCCESS;
}

void ff_dnn_free_model_tf(DNNModel **model)
{
    TFModel *tf_model;

    if (*model){
        tf_model = (TFModel *)(*model)->model;
        if (tf_model->graph){
	    tf_model->tffns->TF_DeleteGraph(tf_model->graph);
        }
        if (tf_model->session){
            tf_model->tffns->TF_CloseSession(tf_model->session, tf_model->status);
            tf_model->tffns->TF_DeleteSession(tf_model->session, tf_model->status);
        }
        if (tf_model->status){
            tf_model->tffns->TF_DeleteStatus(tf_model->status);
        }
        if (tf_model->input_tensor){
            tf_model->tffns->TF_DeleteTensor(tf_model->input_tensor);
        }
        if (tf_model->output_tensors) {
            for (uint32_t i = 0; i < tf_model->nb_output; ++i) {
                if (tf_model->output_tensors[i]) {
                    tf_model->tffns->TF_DeleteTensor(tf_model->output_tensors[i]);
                    tf_model->output_tensors[i] = NULL;
                }
            }
        }

        av_freep(&tf_model->tffns);
        TF_FREE_FUNC(tf_model->libtensorflow);
        av_freep(&tf_model->outputs);
        av_freep(&tf_model->output_tensors);
        av_freep(&tf_model);
        av_freep(model);
    }
}
