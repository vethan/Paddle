/* Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include "paddle/fluid/framework/generator.h"
#include "paddle/fluid/operators/dropout_impl_util.h"
#include "paddle/fluid/operators/fused/fused_dropout_act_bias.h"
#include "paddle/fluid/operators/fused/fused_layernorm_residual_dropout_bias.h"
#include "paddle/fluid/operators/fused/fused_residual_dropout_bias.h"
#include "paddle/phi/kernels/funcs/functors.h"

namespace paddle {
namespace operators {

/**
 * Support two Dropouts in the use senarieo.
 * This warpper can be used in FFN op.
 * The DropoutParam will be used in the fused_dropout_act_bias,
 * fused_residual_dropout_bias(pre_layer_norm=ture) or
 * fused_layernorm_residual_dropout_bias(pre_layer_norm=false).
 */
struct DropoutParam {
  uint64_t seed;
  float dropout_prob;
  bool is_upscale_in_train;
  bool is_test;
  bool fix_seed;
  int increment;
  const framework::Tensor* tensor_seed;
  int seed_val;

  DropoutParam() {
    fix_seed = false;
    seed = 0;
    is_test = false;
    is_upscale_in_train = false;
    dropout_prob = 0.5;
    tensor_seed = nullptr;
    seed_val = 0;
  }

  DropoutParam(bool fix_seed_,
               uint64_t seed_,
               bool is_test_,
               bool is_upscale_in_train_,
               float dropout_prob_,
               const framework::Tensor* tensor_seed_,
               int seed_val_) {
    fix_seed = fix_seed_;
    seed = seed_;
    is_test = is_test_;
    is_upscale_in_train = is_upscale_in_train_;
    dropout_prob = dropout_prob_;
    tensor_seed = tensor_seed_;
    seed_val = seed_val_;
  }

  /**
   * dropout_index: can be 0, 1, 2. 0 means there is only one dropout,
   * 1 and 2 represent two dropout, the parameter name of dropout
   * will be "dropout" + dropout_index + param name, such as dropout1_seed,
   * dropout1_is_test.
   */
  DropoutParam(const framework::ExecutionContext& context,
               const int dropout_index) {
    std::string pre_fix = "dropout";
    std::string str_index = std::to_string(dropout_index);
    if (dropout_index > 0) {
      pre_fix = pre_fix + str_index + "_";
    } else {
      pre_fix = pre_fix + "_";
    }
    dropout_prob = context.Attr<float>(pre_fix + "rate");
    auto& dropout_implementation =
        context.Attr<std::string>(pre_fix + "implementation");
    is_upscale_in_train = (dropout_implementation == "upscale_in_train");
    is_test = context.Attr<bool>("is_test");
    fix_seed = context.Attr<bool>(pre_fix + "fix_seed");

    std::string str_seed = "Dropout";
    if (dropout_index > 0) {
      str_seed = str_seed + str_index + "Seed";
    } else {
      str_seed = str_seed + "Seed";
    }
    tensor_seed =
        context.HasInput(str_seed) ? context.Input<Tensor>(str_seed) : nullptr;
    seed_val = context.Attr<int>(pre_fix + "seed");
  }

  int UpdateSeedAndIncrement(const platform::CUDADeviceContext& ctx,
                             const int offset) {
    uint64_t tmp_increment;
    GetSeedDataAndIncrement(
        ctx, tensor_seed, fix_seed, seed_val, offset, &seed, &tmp_increment);
    increment = static_cast<int>(tmp_increment);
    return increment;
  }
};

template <typename T, typename MaskType>
class FusedDropoutHelper {
 private:
  int GetIncrement(const platform::CUDADeviceContext& ctx) {
    const int VecSize = MAX_CACHE_BYTES / sizeof(T);
    const int real_vec_size = cols_ % VecSize == 0 ? VecSize : 1;
    auto config = Get1DBlocksAnd2DGrids(ctx,
                                        static_cast<uint64_t>(rows_),
                                        static_cast<uint64_t>(cols_),
                                        real_vec_size);
    int increment = ((cols_ - 1) / (config.thread_per_block.x *
                                    config.block_per_grid.x * real_vec_size) +
                     1) *
                    real_vec_size;
    increment = dropout_param_.UpdateSeedAndIncrement(ctx, increment);
    return increment;
  }

 public:
  FusedDropoutHelper() {}
  FusedDropoutHelper(const platform::CUDADeviceContext& ctx,
                     const int rows,
                     const int cols,
                     const DropoutParam& dropout_param) {
    rows_ = rows;
    cols_ = cols;
    dropout_param_ = dropout_param;
  }

  // out = residual + dropout( src + bias )
  void ResidualDropoutBias(const platform::CUDADeviceContext& ctx,
                           const T* src,
                           const T* residual,
                           const T* bias,
                           T* out,
                           MaskType* mask) {
    auto increment = GetIncrement(ctx);
    LaunchResidualDropoutBias<T, MaskType>(rows_,
                                           cols_,
                                           increment,
                                           dropout_param_.seed,
                                           dropout_param_.dropout_prob,
                                           dropout_param_.is_test,
                                           dropout_param_.is_upscale_in_train,
                                           src,
                                           residual,
                                           bias,
                                           mask,
                                           out,
                                           ctx);
  }

  void ResidualDropoutBiasGrad(const platform::CUDADeviceContext& ctx,
                               const T* d_out,
                               const MaskType* mask,
                               T* d_src,
                               T* d_residual,
                               T* d_bias) {
    LaunchResidualDropoutBiasGrad<T, uint8_t>(
        d_out,
        mask,
        dropout_param_.dropout_prob,
        dropout_param_.is_upscale_in_train,
        rows_,
        cols_,
        d_src,
        d_bias,
        ctx);
    if (d_residual) {
      memory::Copy(ctx.GetPlace(),
                   d_residual,
                   ctx.GetPlace(),
                   d_out,
                   rows_ * cols_ * sizeof(T),
                   ctx.stream());
    }
  }

  // out = dropout(activation(src + bias))
  void DropoutActBias(const platform::CUDADeviceContext& ctx,
                      const T* src,
                      const T* bias,
                      const std::string& act_method,
                      T* out,
                      MaskType* mask) {
    auto increment = GetIncrement(ctx);
    if (act_method == "gelu") {
      GeluFunctor<T> gelu;
      LaunchDropoutActBias<T, MaskType, GeluFunctor<T>>(
          gelu,
          dropout_param_.seed,
          rows_,
          cols_,
          dropout_param_.increment,
          dropout_param_.dropout_prob,
          dropout_param_.is_upscale_in_train,
          dropout_param_.is_test,
          src,
          bias,
          out,
          mask,
          ctx);
    } else if (act_method == "relu") {
      phi::funcs::ReluFunctor<T> relu;
      LaunchDropoutActBias<T, MaskType, phi::funcs::ReluFunctor<T>>(
          relu,
          dropout_param_.seed,
          rows_,
          cols_,
          increment,
          dropout_param_.dropout_prob,
          dropout_param_.is_upscale_in_train,
          dropout_param_.is_test,
          src,
          bias,
          out,
          mask,
          ctx);
    } else {
      PADDLE_THROW(platform::errors::InvalidArgument(
          "Currently only supports gelu or relu activation functions!"));
    }
  }

  void DropoutActBiasGrad(const platform::CUDADeviceContext& ctx,
                          const T* dout,
                          const T* src,
                          const T* bias,
                          const MaskType* mask,
                          T* d_src,
                          T* d_bias,
                          const std::string& act_method) {
    if (act_method == "gelu") {
      GeluGradFunctor<T> gelu_grad;
      LaunchDropoutActBiasGrad<T, MaskType, GeluGradFunctor<T>>(
          gelu_grad,
          dout,
          mask,
          src,
          bias,
          dropout_param_.dropout_prob,
          dropout_param_.is_upscale_in_train,
          rows_,
          cols_,
          d_src,
          d_bias,
          ctx);
    } else if (act_method == "relu") {
      phi::funcs::ReluGradFunctor<T> relu_grad;
      LaunchDropoutActBiasGrad<T, MaskType, phi::funcs::ReluGradFunctor<T>>(
          relu_grad,
          dout,
          mask,
          src,
          bias,
          dropout_param_.dropout_prob,
          dropout_param_.is_upscale_in_train,
          rows_,
          cols_,
          d_src,
          d_bias,
          ctx);
    } else {
      PADDLE_THROW(platform::errors::InvalidArgument(
          "Currently only supports gelu or relu activation functions!"));
    }
  }

 protected:
  int rows_;
  int cols_;
  DropoutParam dropout_param_;
};

template <typename T, typename MaskType>
class FusedDropoutLayerNormHelper : public FusedDropoutHelper<T, MaskType> {
 public:
  FusedDropoutLayerNormHelper() {}
  FusedDropoutLayerNormHelper(const int rows,
                              const int cols,
                              const float epsilon) {
    using U = LayerNormParamType<T>;
    this->rows_ = rows;
    this->cols_ = cols;
    epsilon_ = epsilon;
  }

  FusedDropoutLayerNormHelper(const platform::CUDADeviceContext& ctx,
                              const int rows,
                              const int cols,
                              const DropoutParam& dropout_param,
                              const float epsilon)
      : FusedDropoutHelper<T, MaskType>(ctx, rows, cols, dropout_param) {
    using U = LayerNormParamType<T>;
    epsilon_ = epsilon;
  }

  // call layer_norm
  void LayerNorm(const platform::CUDADeviceContext& ctx,
                 const T* src,
                 const LayerNormParamType<T>* gamma,
                 const LayerNormParamType<T>* beta,
                 T* out,
                 LayerNormParamType<T>* mean,
                 LayerNormParamType<T>* variance) {
    using U = LayerNormParamType<T>;
    switch (GetDesiredBlockDim(this->cols_)) {
      FIXED_BLOCK_DIM_CASE(
          LayerNormForward<T, U, kBlockDim>
          <<<this->rows_, kBlockDim, 0, ctx.stream()>>>(
              src, gamma, beta, out, mean, variance, epsilon_, this->cols_));
    }
  }

  void LayerNormGrad(const platform::CUDADeviceContext& ctx,
                     const T* dout,
                     const T* src,
                     const LayerNormParamType<T>* gamma,
                     const LayerNormParamType<T>* mean,
                     const LayerNormParamType<T>* variance,
                     T* d_src,
                     LayerNormParamType<T>* d_scale,
                     LayerNormParamType<T>* d_bias) {
    using U = LayerNormParamType<T>;
    LayerNormBackward<T, U>(src,
                            dout,
                            gamma,
                            mean,
                            variance,
                            d_src,
                            d_scale,
                            d_bias,
                            epsilon_,
                            this->rows_,
                            this->cols_,
                            ctx);
  }

  // out = layernorm(residual + dropout(src + bias))
  template <typename P = LayerNormParamType<T>, bool is_same_type = false>
  void LayernormResidualDropoutBias(const platform::CUDADeviceContext& ctx,
                                    const T* src,
                                    const T* residual,
                                    const T* bias,
                                    const P* gamma,
                                    const P* beta,
                                    T* dropout_out,
                                    MaskType* mask,
                                    T* out,
                                    LayerNormParamType<T>* mean,
                                    LayerNormParamType<T>* variance) {
    using U = LayerNormParamType<T>;
    int vec_size = MAX_CACHE_BYTES / sizeof(T);
    if (this->cols_ % vec_size != 0) {
      vec_size = 1;
    }
    int threads = GetDesiredBlockDim(this->cols_ / vec_size);
    int increment = ((this->cols_ - 1) / (threads * vec_size) + 1) * vec_size;
    increment = this->dropout_param_.UpdateSeedAndIncrement(ctx, increment);
    LaunchLayernormResidualDropoutBias<T, MaskType, U, is_same_type>(
        this->rows_,
        this->cols_,
        increment,
        this->dropout_param_.seed,
        this->dropout_param_.dropout_prob,
        epsilon_,
        this->dropout_param_.is_upscale_in_train,
        this->dropout_param_.is_test,
        src,
        residual,
        bias,
        gamma,
        beta,
        mask,
        dropout_out,
        out,
        mean,
        variance,
        ctx);
  }

  template <typename P = LayerNormParamType<T>, bool is_same_type = false>
  void LayernormResidualDropoutBiasGrad(const platform::CUDADeviceContext& ctx,
                                        const T* d_out,
                                        const T* layernorm_src,
                                        const MaskType* mask,
                                        const P* gamma,
                                        const LayerNormParamType<T>* mean,
                                        const LayerNormParamType<T>* variance,
                                        T* d_layernorm_src,
                                        P* d_scale,
                                        P* d_layernorm_bias,
                                        T* d_dropout_src,
                                        T* d_bias,
                                        T* d_residual) {
    using U = LayerNormParamType<T>;
    bool can_call_1024_kernel = false;
    // Fast impl for cases when cols is 1024 and linear_bias is nullptr.
    // In fact, linear_bias is not nullptr is also feasible for impl.
    // Here, we do not support it.
    if (this->cols_ == 1024 && d_bias == nullptr && d_scale != nullptr &&
        d_layernorm_bias != nullptr && sizeof(T) <= 4) {
      can_call_1024_kernel = true;
    }
    VLOG(6) << "LaunchLayernormResidualDropoutGrad = " << can_call_1024_kernel;

    if (can_call_1024_kernel) {
      LaunchLayernormResidualDropoutGrad<T, U, MaskType, is_same_type>(
          ctx,
          this->rows_,
          this->cols_,
          epsilon_,
          this->dropout_param_.dropout_prob,
          this->dropout_param_.is_upscale_in_train,
          d_out,
          layernorm_src,
          gamma,
          mean,
          variance,
          mask,
          d_scale,
          d_layernorm_bias,
          d_residual,
          d_dropout_src);
    } else {
      LayerNormBackward<T, U, is_same_type>(layernorm_src,
                                            d_out,
                                            gamma,
                                            mean,
                                            variance,
                                            d_layernorm_src,
                                            d_scale,
                                            d_layernorm_bias,
                                            epsilon_,
                                            this->rows_,
                                            this->cols_,
                                            ctx);
      this->ResidualDropoutBiasGrad(
          ctx, d_layernorm_src, mask, d_dropout_src, d_residual, d_bias);
    }
  }

 protected:
  float epsilon_;
};

}  // namespace operators
}  // namespace paddle
