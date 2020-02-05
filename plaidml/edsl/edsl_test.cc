// Copyright 2019 Intel Corporation.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <variant>

#include "llvm/ADT/StringRef.h"

#include "plaidml/edsl/autodiff.h"
#include "plaidml/edsl/edsl.h"
#include "plaidml/exec/exec.h"
#include "pmlc/util/env.h"
#include "pmlc/util/logging.h"

using llvm::StringRef;
using ::testing::ContainerEq;
using ::testing::Eq;

namespace plaidml::edsl {

bool operator==(const Program& lhs, const std::string& rhs) {  //
  return StringRef(lhs.str()).trim() == StringRef(rhs).trim();
}

namespace {

Tensor Dot(const Tensor& X, const Tensor& Y) {
  TensorDim I, J, K;
  TensorIndex i("i"), j("j"), k("k");
  X.bind_dims(I, K);
  Y.bind_dims(K, J);
  auto R = TensorOutput(I, J);
  R(i, j) += X(i, k) * Y(k, j);
  return R;
}

Tensor Relu(const Tensor& I) { return select(I < 0.0, Tensor{0.0}, I); }

Tensor Softmax(const Tensor& X) {
  TensorDim I, J;
  TensorIndex i, j;
  X.bind_dims(I, J);
  auto M = TensorOutput(I, 1);
  M(i, 0) >= X(i, j);
  auto E = exp(X - M);
  auto N = TensorOutput(I, 1);
  N(i, 0) += E(i, j);
  return E / N;
}

using MultiBuffer = std::variant<  //
    std::vector<float>,            //
    std::vector<double>,           //
    std::vector<int>,              //
    std::vector<std::int32_t>,     //
    std::vector<std::int64_t>,     //
    std::vector<std::uint32_t>,    //
    std::vector<std::uint64_t>>;

class CppEdsl : public ::testing::Test {
 protected:
  template <typename T>
  void compareBuffers(plaidml::View view, const std::vector<T>& expected) {
    ASSERT_THAT(view.size(), expected.size() * sizeof(expected[0]));
    auto data = reinterpret_cast<T*>(view.data());
    std::vector<T> actual(data, data + expected.size());
    EXPECT_THAT(actual, ContainerEq(expected));
  }

  void checkProgram(                                   //
      const Program& program,                          //
      const std::map<TensorRef, MultiBuffer>& inputs,  //
      const std::map<TensorRef, MultiBuffer>& expected) {
#if !defined(_WIN32)
    auto binder = exec::Binder(program);
    auto executable = binder.compile();
    for (const auto& kvp : inputs) {
      std::visit([&](auto&& vec) { binder.input(kvp.first).copy_from(vec.data()); }, kvp.second);
    }
    executable->run();
    for (auto kvp : expected) {
      auto view = binder.output(kvp.first).mmap_current();
      std::visit([&](auto&& vec) { compareBuffers(view, vec); }, kvp.second);
    }
#endif
  }

  void runProgram(const Program& program) {
#if !defined(_WIN32)
    exec::Binder(program).compile()->run();
#endif
  }
};

TEST_F(CppEdsl, HigherPrecisionInvalidNegative) {
  auto A = Placeholder(DType::FLOAT32, {3, 3});
  auto C = A * (-2);

  EXPECT_ANY_THROW({ Program("higher_precision_constants", {C}, DType::FLOAT64, DType::UINT64); });
}

TEST_F(CppEdsl, HigherPrecisionConstants) {
  auto A = Placeholder(DType::FLOAT32, {3, 3});
  auto C = A + 1 + 2.0;

  Program program("higher_precision_constants", {C}, DType::FLOAT64, DType::UINT64);

  EXPECT_THAT(program, Eq(R"#(
!u64 = type tensor<!eltwise.u64>
!f64 = type tensor<!eltwise.f64>
module {
  func @higher_precision_constants(%arg0: tensor<3x3x!eltwise.f32>) -> tensor<3x3x!eltwise.f64> {
    %c1 = "eltwise.sconst"() {value = 1 : i64} : () -> !u64
    %cst = "eltwise.sconst"() {value = 2.000000e+00 : f64} : () -> !f64
    %0 = "eltwise.add"(%arg0, %c1) : (tensor<3x3x!eltwise.f32>, !u64) -> tensor<3x3x!eltwise.f32>
    %1 = "eltwise.add"(%0, %cst) : (tensor<3x3x!eltwise.f32>, !f64) -> tensor<3x3x!eltwise.f64>
    return %1 : tensor<3x3x!eltwise.f64>
  }
}
)#"));

  std::vector<float> A_input{1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<double> C_output{4, 5, 6, 7, 8, 9, 10, 11, 12};
  checkProgram(program, {{A, A_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, Cast) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = cast(A, DType::UINT32);
  Program program("cast", {B});

  std::vector<std::uint64_t> A_input{1,
                                     2,
                                     3,
                                     4,
                                     5,
                                     6 + (1UL << 12),
                                     7 + (1UL << 24),
                                     8 + (1UL << 31),  //
                                     (1ULL << 32) - 1};
  std::vector<std::uint32_t> B_output{1,
                                      2,
                                      3,
                                      4,
                                      5,
                                      6 + (1UL << 12),
                                      7 + (1UL << 24),
                                      8 + (1UL << 31),  //
                                      (1ULL << 32) - 1};
  checkProgram(program, {{A, A_input}}, {{B, B_output}});
}

TEST_F(CppEdsl, BitOr) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = Placeholder(DType::UINT64, {3, 3});
  auto C = A | B;
  Program program("bit_or", {C});

  std::vector<std::uint64_t> A_input{1, 2, 3,  //
                                     4, 5, 6,  //
                                     7, 8, 9};
  std::vector<std::uint64_t> B_input{10, 11, 12,  //
                                     13, 14, 15,  //
                                     16, 17, 18};
  std::vector<std::uint64_t> C_output{1 | 10, 2 | 11, 3 | 12,  //
                                      4 | 13, 5 | 14, 6 | 15,  //
                                      7 | 16, 8 | 17, 9 | 18};
  checkProgram(program, {{A, A_input}, {B, B_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, BitLeft) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = Placeholder(DType::UINT64, {3, 3});
  auto C = A << B;
  Program program("bit_left", {C});

  std::vector<std::uint64_t> A_input{1, 2, 3,  //
                                     4, 5, 6,  //
                                     7, 8, 9};
  std::vector<std::uint64_t> B_input{10, 11, 12,  //
                                     13, 14, 15,  //
                                     16, 17, 18};
  std::vector<std::uint64_t> C_output{1 << 10, 2 << 11, 3 << 12,  //
                                      4 << 13, 5 << 14, 6 << 15,  //
                                      7 << 16, 8 << 17, 9 << 18};
  checkProgram(program, {{A, A_input}, {B, B_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, BitRightTensor) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = Placeholder(DType::UINT64, {3, 3});
  auto C = A >> B;
  Program program("bit_right_tensor", {C});

  std::vector<std::uint64_t> A_input{1 << 10, 2 << 11, 3 << 12,  //
                                     4 << 13, 5 << 14, 6 << 15,  //
                                     7 << 16, 8 << 17, 9 << 18};
  std::vector<std::uint64_t> B_input{10, 11, 12,  //
                                     13, 14, 15,  //
                                     16, 17, 18};
  std::vector<std::uint64_t> C_output{1, 2, 3,  //
                                      4, 5, 6,  //
                                      7, 8, 9};
  checkProgram(program, {{A, A_input}, {B, B_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, BitRightScalar) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = A >> 9;
  Program program("bit_right_scalar", {B});

  std::vector<std::uint64_t> A_input{1 << 10, 2 << 11, 3 << 12,  //
                                     4 << 13, 5 << 14, 6 << 15,  //
                                     7 << 16, 8 << 17, 9 << 18};
  std::vector<std::uint64_t> B_output{1 << 1, 2 << 2, 3 << 3,  //
                                      4 << 4, 5 << 5, 6 << 6,  //
                                      7 << 7, 8 << 8, 9 << 9};
  checkProgram(program, {{A, A_input}}, {{B, B_output}});
}

TEST_F(CppEdsl, BitXor) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = Placeholder(DType::UINT64, {3, 3});
  auto C = A ^ B;
  Program program("bit_xor", {C});

  std::vector<std::uint64_t> A_input{1, 2, 3,  //
                                     4, 5, 6,  //
                                     7, 8, 9};
  std::vector<std::uint64_t> B_input{10, 11, 12,  //
                                     13, 14, 15,  //
                                     16, 17, 18};
  std::vector<std::uint64_t> C_output{1 ^ 10, 2 ^ 11, 3 ^ 12,  //
                                      4 ^ 13, 5 ^ 14, 6 ^ 15,  //
                                      7 ^ 16, 8 ^ 17, 9 ^ 18};
  checkProgram(program, {{A, A_input}, {B, B_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, BroadcastCmp) {
  auto A = Placeholder(DType::UINT64, {3, 4});
  auto B = Placeholder(DType::UINT64, {3, 1});
  auto C = cast(A >= B, DType::UINT64);
  Program program("broadcast_cmp", {C});

  std::vector<std::uint64_t> A_input = {0, 1, 2,  3,  //
                                        4, 5, 6,  7,  //
                                        8, 9, 10, 11};
  std::vector<std::uint64_t> B_input = {0, 6, 12};
  std::vector<std::uint64_t> C_output = {1, 1, 1, 1,  //
                                         0, 0, 1, 1,  //
                                         0, 0, 0, 0};
  checkProgram(program, {{A, A_input}, {B, B_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, Add) {
  auto A = Placeholder(DType::UINT64, {3, 3});
  auto B = Placeholder(DType::UINT64, {3, 3});
  auto C = A + B;
  Program program("add", {C});

  std::vector<std::uint64_t> A_input = {
      1,
      2,
      3,
      4,
      5,
      6 + (1UL << 12),
      7 + (1UL << 24),
      8 + (1ULL << 32),
      9 + (1ULL << 40)  //
  };

  std::vector<std::uint64_t> B_input = {1,
                                        2 + (1UL << 12),
                                        3,
                                        4 + (1UL << 24),
                                        5,
                                        6 + (1ULL << 32),
                                        7,
                                        8 + (1ULL << 40),  //
                                        9};

  std::vector<std::uint64_t> C_output = {2,
                                         4 + (1UL << 12),
                                         6,
                                         8 + (1UL << 24),
                                         10,
                                         12 + (1UL << 12) + (1ULL << 32),
                                         14 + (1UL << 24),
                                         16 + (1ULL << 32) + (1ULL << 40),
                                         18 + (1ULL << 40)};

  checkProgram(program, {{A, A_input}, {B, B_input}}, {{C, C_output}});
}

TEST_F(CppEdsl, Dot) {
  auto A = Placeholder(DType::FLOAT32, {3, 3});
  auto B = Placeholder(DType::FLOAT32, {3, 3});
  auto C = Dot(A, B);
  Program program("dot", {C});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2) -> (d0, d1)>
#map1 = affine_map<(d0, d1, d2) -> (d0, d2)>
#map2 = affine_map<(d0, d1, d2) -> (d2, d1)>


!f32 = type tensor<!eltwise.f32>
module {
  func @dot(%arg0: tensor<3x3x!eltwise.f32>, %arg1: tensor<3x3x!eltwise.f32>) -> tensor<3x3x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg1, %arg0 {idxs = ["i", "j", "k"], sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<3x3x!eltwise.f32>, tensor<3x3x!eltwise.f32> -> tensor<3x3x!eltwise.f32>
    return %0 : tensor<3x3x!eltwise.f32>
  }
}
)#"));

  std::vector<float> input = {
      1.0f, 2.0f, 3.0f,  //
      4.0f, 5.0f, 6.0f,  //
      7.0f, 8.0f, 9.0f,  //
  };

  std::vector<float> expected = {
      30.0f,  36.0f,  42.0f,   //
      66.0f,  81.0f,  96.0f,   //
      102.0f, 126.0f, 150.0f,  //
  };
  checkProgram(program, {{A, input}, {B, input}}, {{C, expected}});
}

TEST_F(CppEdsl, DoubleDot) {
  auto A = Placeholder(DType::FLOAT32, {10, 20});
  auto B = Placeholder(DType::FLOAT32, {20, 30});
  auto C = Placeholder(DType::FLOAT32, {30, 40});
  Program program("double_dot", {Dot(Dot(A, B), C)});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2) -> (d0, d1)>
#map1 = affine_map<(d0, d1, d2) -> (d0, d2)>
#map2 = affine_map<(d0, d1, d2) -> (d2, d1)>


!f32 = type tensor<!eltwise.f32>
module {
  func @double_dot(%arg0: tensor<30x40x!eltwise.f32>, %arg1: tensor<20x30x!eltwise.f32>, %arg2: tensor<10x20x!eltwise.f32>) -> tensor<10x40x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg2, %arg1 {idxs = ["i", "j", "k"], sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<10x20x!eltwise.f32>, tensor<20x30x!eltwise.f32> -> tensor<10x30x!eltwise.f32>
    %1 = tile.cion add, mul, %cst, %0, %arg0 {idxs = ["i", "j", "k"], sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<10x30x!eltwise.f32>, tensor<30x40x!eltwise.f32> -> tensor<10x40x!eltwise.f32>
    return %1 : tensor<10x40x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, EltwiseAdd) {
  auto A = Placeholder(DType::FLOAT32, {10, 20});
  auto B = Placeholder(DType::FLOAT32, {10, 20});
  Program program("eltwise_add", {A + B});
  EXPECT_THAT(program, Eq(R"#(
module {
  func @eltwise_add(%arg0: tensor<10x20x!eltwise.f32>, %arg1: tensor<10x20x!eltwise.f32>) -> tensor<10x20x!eltwise.f32> {
    %0 = "eltwise.add"(%arg1, %arg0) : (tensor<10x20x!eltwise.f32>, tensor<10x20x!eltwise.f32>) -> tensor<10x20x!eltwise.f32>
    return %0 : tensor<10x20x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, Relu) {
  auto A = Placeholder(DType::FLOAT32, {10, 20});
  Program program("relu", {Relu(A)});
  EXPECT_THAT(program, Eq(R"#(
!f32 = type tensor<!eltwise.f32>
module {
  func @relu(%arg0: tensor<10x20x!eltwise.f32>) -> tensor<10x20x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = "eltwise.cmp_lt"(%arg0, %cst) : (tensor<10x20x!eltwise.f32>, !f32) -> tensor<10x20x!eltwise.u1>
    %1 = "eltwise.select"(%0, %cst, %arg0) : (tensor<10x20x!eltwise.u1>, !f32, tensor<10x20x!eltwise.f32>) -> tensor<10x20x!eltwise.f32>
    return %1 : tensor<10x20x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, MnistMlp) {
  // model.add(Dense(512, activation='relu', input_shape=(784,)))
  auto input = Placeholder(DType::FLOAT32, {1, 784});
  auto kernel1 = Placeholder(DType::FLOAT32, {784, 512});
  auto bias1 = Placeholder(DType::FLOAT32, {512});
  auto dense1 = Relu(Dot(input, kernel1) + bias1);
  // model.add(Dense(512, activation='relu'))
  auto kernel2 = Placeholder(DType::FLOAT32, {512, 512});
  auto bias2 = Placeholder(DType::FLOAT32, {512});
  auto dense2 = Relu(Dot(dense1, kernel2) + bias2);
  // model.add(Dense(10, activation='softmax'))
  auto kernel3 = Placeholder(DType::FLOAT32, {512, 10});
  auto bias3 = Placeholder(DType::FLOAT32, {10});
  auto dense3 = Softmax(Dot(dense2, kernel3) + bias3);
  Program program("mnist_mlp", {dense3});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2) -> (d0, d1)>
#map1 = affine_map<(d0, d1, d2) -> (d0, d2)>
#map2 = affine_map<(d0, d1, d2) -> (d2, d1)>
#map3 = affine_map<(d0, d1) -> (d0, 0)>
#map4 = affine_map<(d0, d1) -> (d0, d1)>


!f32 = type tensor<!eltwise.f32>
module {
  func @mnist_mlp(%arg0: tensor<10x!eltwise.f32>, %arg1: tensor<512x10x!eltwise.f32>, %arg2: tensor<512x!eltwise.f32>, %arg3: tensor<512x512x!eltwise.f32>, %arg4: tensor<512x!eltwise.f32>, %arg5: tensor<784x512x!eltwise.f32>, %arg6: tensor<1x784x!eltwise.f32>) -> tensor<1x10x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg6, %arg5 {idxs = ["i", "j", "k"], sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x784x!eltwise.f32>, tensor<784x512x!eltwise.f32> -> tensor<1x512x!eltwise.f32>
    %1 = "eltwise.add"(%0, %arg4) : (tensor<1x512x!eltwise.f32>, tensor<512x!eltwise.f32>) -> tensor<1x512x!eltwise.f32>
    %2 = "eltwise.cmp_lt"(%1, %cst) : (tensor<1x512x!eltwise.f32>, !f32) -> tensor<1x512x!eltwise.u1>
    %3 = "eltwise.select"(%2, %cst, %1) : (tensor<1x512x!eltwise.u1>, !f32, tensor<1x512x!eltwise.f32>) -> tensor<1x512x!eltwise.f32>
    %4 = tile.cion add, mul, %cst, %3, %arg3 {idxs = ["i", "j", "k"], sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x512x!eltwise.f32>, tensor<512x512x!eltwise.f32> -> tensor<1x512x!eltwise.f32>
    %5 = "eltwise.add"(%4, %arg2) : (tensor<1x512x!eltwise.f32>, tensor<512x!eltwise.f32>) -> tensor<1x512x!eltwise.f32>
    %6 = "eltwise.cmp_lt"(%5, %cst) : (tensor<1x512x!eltwise.f32>, !f32) -> tensor<1x512x!eltwise.u1>
    %7 = "eltwise.select"(%6, %cst, %5) : (tensor<1x512x!eltwise.u1>, !f32, tensor<1x512x!eltwise.f32>) -> tensor<1x512x!eltwise.f32>
    %8 = tile.cion add, mul, %cst, %7, %arg1 {idxs = ["i", "j", "k"], sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x512x!eltwise.f32>, tensor<512x10x!eltwise.f32> -> tensor<1x10x!eltwise.f32>
    %9 = "eltwise.add"(%8, %arg0) : (tensor<1x10x!eltwise.f32>, tensor<10x!eltwise.f32>) -> tensor<1x10x!eltwise.f32>
    %10 = tile.cion max, none, %cst, %9 {sink = #map3, srcs = [#map4]} : !f32, tensor<1x10x!eltwise.f32> -> tensor<1x1x!eltwise.f32>
    %11 = "eltwise.sub"(%9, %10) : (tensor<1x10x!eltwise.f32>, tensor<1x1x!eltwise.f32>) -> tensor<1x10x!eltwise.f32>
    %12 = "eltwise.exp"(%11) : (tensor<1x10x!eltwise.f32>) -> tensor<1x10x!eltwise.f32>
    %13 = tile.cion add, none, %cst, %12 {sink = #map3, srcs = [#map4]} : !f32, tensor<1x10x!eltwise.f32> -> tensor<1x1x!eltwise.f32>
    %14 = "eltwise.div"(%12, %13) : (tensor<1x10x!eltwise.f32>, tensor<1x1x!eltwise.f32>) -> tensor<1x10x!eltwise.f32>
    return %14 : tensor<1x10x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

Tensor Convolution2(const Tensor& I, const Tensor& K) {
  TensorDim CI, CO, K0, K1, N, X0, X1;
  TensorIndex n, x0, x1, co, ci, k0, k1;
  I.bind_dims(N, X0, X1, CI);
  K.bind_dims(K0, K1, CI, CO);
  auto R = TensorOutput(N, X0 - (K0 - 1), X1 - (K1 - 1), CO);
  R(n, x0, x1, co) += I(n, x0 + k0 - (K0 / 2), x1 + k1 - (K1 / 2), ci) * K(k0, k1, ci, co);
  return R;
}

TEST_F(CppEdsl, Convolution) {
  auto I = Placeholder(DType::FLOAT32, {1, 224, 224, 1});
  auto K = Placeholder(DType::FLOAT32, {3, 3, 1, 32});
  Program program("convolution", {Convolution2(I, K)});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d0, d1, d2, d3)>
#map1 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d0, d1 + d4 - 1, d2 + d5 - 1, d6)>
#map2 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d4, d5, d6, d3)>


!f32 = type tensor<!eltwise.f32>
module {
  func @convolution(%arg0: tensor<3x3x1x32x!eltwise.f32>, %arg1: tensor<1x224x224x1x!eltwise.f32>) -> tensor<1x222x222x32x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg1, %arg0 {sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x224x224x1x!eltwise.f32>, tensor<3x3x1x32x!eltwise.f32> -> tensor<1x222x222x32x!eltwise.f32>
    return %0 : tensor<1x222x222x32x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

Tensor MaxPooling2(const Tensor& I) {
  TensorDim N, X0, X1, C;
  TensorIndex n, x0, x1, i, j, c;
  I.bind_dims(N, X0, X1, C);
  auto R = TensorOutput(N, (X0 + 1) / 2, (X1 + 1) / 2, C);
  R(n, x0, x1, c) >= I(n, 2 * x0 + i, 2 * x1 + j, c);
  R.add_constraints({i < 2, j < 2});
  return R;
}

Tensor Flatten(const Tensor& X) {
  std::vector<TensorDim> X_dims(X.rank());
  X.bind_dims(X_dims);
  if (X_dims.empty()) {
    return X;
  }
  TensorDim product{1};
  for (size_t i = 1; i < X_dims.size() - 1; i++) {
    product = product * X_dims[i];
  }
  return reshape(X, {TensorDim{1}, product});
}

TEST_F(CppEdsl, MnistCnn) {
  // model.add(Conv2D(32, kernel_size=(3, 3), activation='relu', input_shape=input_shape))
  auto input = Placeholder(DType::FLOAT32, {1, 224, 224, 1});
  auto kernel1 = Placeholder(DType::FLOAT32, {3, 3, 1, 32});
  auto bias1 = Placeholder(DType::FLOAT32, {32});
  auto conv1 = Relu(Convolution2(input, kernel1) + bias1);
  // model.add(Conv2D(64, (3, 3), activation='relu'))
  auto kernel2 = Placeholder(DType::FLOAT32, {3, 3, 32, 64});
  auto bias2 = Placeholder(DType::FLOAT32, {64});
  auto conv2 = Relu(Convolution2(conv1, kernel2) + bias2);
  // model.add(MaxPooling2D(pool_size=(2, 2)))
  auto pool1 = MaxPooling2(conv2);
  // model.add(Flatten())
  auto flat = Flatten(pool1);
  EXPECT_THAT(flat.compute_shape(), Eq(LogicalShape(DType::FLOAT32, {1, 12100})));
  // model.add(Dense(128, activation='relu'))
  auto kernel3 = Placeholder(DType::FLOAT32, {12100, 128});
  auto bias3 = Placeholder(DType::FLOAT32, {128});
  auto dense1 = Relu(Dot(flat, kernel3) + bias3);
  const std::int64_t kNumClasses = 100;
  // model.add(Dense(num_classes, activation='softmax'))
  auto kernel4 = Placeholder(DType::FLOAT32, {128, kNumClasses});
  auto bias4 = Placeholder(DType::FLOAT32, {kNumClasses});
  auto dense2 = Softmax(Dot(dense1, kernel4) + bias4);
  Program program("mnist_cnn", {dense2});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d0, d1, d2, d3)>
#map1 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d0, d1 + d4 - 1, d2 + d5 - 1, d6)>
#map2 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d4, d5, d6, d3)>
#map3 = affine_map<(d0, d1, d2, d3, d4, d5) -> (d0, d1, d2, d3)>
#map4 = affine_map<(d0, d1, d2, d3, d4, d5) -> (d0, d1 * 2 + d4, d2 * 2 + d5, d3)>
#map5 = affine_map<(d0, d1, d2) -> (d0, d1)>
#map6 = affine_map<(d0, d1, d2) -> (d0, d2)>
#map7 = affine_map<(d0, d1, d2) -> (d2, d1)>
#map8 = affine_map<(d0, d1) -> (d0, 0)>
#map9 = affine_map<(d0, d1) -> (d0, d1)>

#set0 = affine_set<(d0, d1, d2, d3, d4, d5) : (d4 >= 0, -d4 + 1 >= 0, d5 >= 0, -d5 + 1 >= 0)>

!f32 = type tensor<!eltwise.f32>
module {
  func @mnist_cnn(%arg0: tensor<100x!eltwise.f32>, %arg1: tensor<128x100x!eltwise.f32>, %arg2: tensor<128x!eltwise.f32>, %arg3: tensor<12100x128x!eltwise.f32>, %arg4: tensor<64x!eltwise.f32>, %arg5: tensor<3x3x32x64x!eltwise.f32>, %arg6: tensor<32x!eltwise.f32>, %arg7: tensor<3x3x1x32x!eltwise.f32>, %arg8: tensor<1x224x224x1x!eltwise.f32>) -> tensor<1x100x!eltwise.f32> {
    %c12100 = tile.affine_const 12100
    %c1 = tile.affine_const 1
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg8, %arg7 {sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x224x224x1x!eltwise.f32>, tensor<3x3x1x32x!eltwise.f32> -> tensor<1x222x222x32x!eltwise.f32>
    %1 = "eltwise.add"(%0, %arg6) : (tensor<1x222x222x32x!eltwise.f32>, tensor<32x!eltwise.f32>) -> tensor<1x222x222x32x!eltwise.f32>
    %2 = "eltwise.cmp_lt"(%1, %cst) : (tensor<1x222x222x32x!eltwise.f32>, !f32) -> tensor<1x222x222x32x!eltwise.u1>
    %3 = "eltwise.select"(%2, %cst, %1) : (tensor<1x222x222x32x!eltwise.u1>, !f32, tensor<1x222x222x32x!eltwise.f32>) -> tensor<1x222x222x32x!eltwise.f32>
    %4 = tile.cion add, mul, %cst, %3, %arg5 {sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x222x222x32x!eltwise.f32>, tensor<3x3x32x64x!eltwise.f32> -> tensor<1x220x220x64x!eltwise.f32>
    %5 = "eltwise.add"(%4, %arg4) : (tensor<1x220x220x64x!eltwise.f32>, tensor<64x!eltwise.f32>) -> tensor<1x220x220x64x!eltwise.f32>
    %6 = "eltwise.cmp_lt"(%5, %cst) : (tensor<1x220x220x64x!eltwise.f32>, !f32) -> tensor<1x220x220x64x!eltwise.u1>
    %7 = "eltwise.select"(%6, %cst, %5) : (tensor<1x220x220x64x!eltwise.u1>, !f32, tensor<1x220x220x64x!eltwise.f32>) -> tensor<1x220x220x64x!eltwise.f32>
    %8 = tile.cion max, none, %cst, %7 {cons = #set0, sink = #map3, srcs = [#map4]} : !f32, tensor<1x220x220x64x!eltwise.f32> -> tensor<1x110x110x64x!eltwise.f32>
    %9 = "tile.reshape"(%8, %c1, %c12100) : (tensor<1x110x110x64x!eltwise.f32>, index, index) -> tensor<1x12100x!eltwise.f32>
    %10 = tile.cion add, mul, %cst, %9, %arg3 {idxs = ["i", "j", "k"], sink = #map5, srcs = [#map6, #map7]} : !f32, tensor<1x12100x!eltwise.f32>, tensor<12100x128x!eltwise.f32> -> tensor<1x128x!eltwise.f32>
    %11 = "eltwise.add"(%10, %arg2) : (tensor<1x128x!eltwise.f32>, tensor<128x!eltwise.f32>) -> tensor<1x128x!eltwise.f32>
    %12 = "eltwise.cmp_lt"(%11, %cst) : (tensor<1x128x!eltwise.f32>, !f32) -> tensor<1x128x!eltwise.u1>
    %13 = "eltwise.select"(%12, %cst, %11) : (tensor<1x128x!eltwise.u1>, !f32, tensor<1x128x!eltwise.f32>) -> tensor<1x128x!eltwise.f32>
    %14 = tile.cion add, mul, %cst, %13, %arg1 {idxs = ["i", "j", "k"], sink = #map5, srcs = [#map6, #map7]} : !f32, tensor<1x128x!eltwise.f32>, tensor<128x100x!eltwise.f32> -> tensor<1x100x!eltwise.f32>
    %15 = "eltwise.add"(%14, %arg0) : (tensor<1x100x!eltwise.f32>, tensor<100x!eltwise.f32>) -> tensor<1x100x!eltwise.f32>
    %16 = tile.cion max, none, %cst, %15 {sink = #map8, srcs = [#map9]} : !f32, tensor<1x100x!eltwise.f32> -> tensor<1x1x!eltwise.f32>
    %17 = "eltwise.sub"(%15, %16) : (tensor<1x100x!eltwise.f32>, tensor<1x1x!eltwise.f32>) -> tensor<1x100x!eltwise.f32>
    %18 = "eltwise.exp"(%17) : (tensor<1x100x!eltwise.f32>) -> tensor<1x100x!eltwise.f32>
    %19 = tile.cion add, none, %cst, %18 {sink = #map8, srcs = [#map9]} : !f32, tensor<1x100x!eltwise.f32> -> tensor<1x1x!eltwise.f32>
    %20 = "eltwise.div"(%18, %19) : (tensor<1x100x!eltwise.f32>, tensor<1x1x!eltwise.f32>) -> tensor<1x100x!eltwise.f32>
    return %20 : tensor<1x100x!eltwise.f32>
  }
}
)#"));
  // TODO: error: failed to legalize operation 'tile.reshape'
  // runProgram(program);
}

Tensor Normalize(const Tensor& X) {
  auto XSqr = X * X;
  auto X_MS = TensorOutput();
  std::vector<TensorIndex> idxs(X.rank());
  X_MS() += XSqr(idxs);
  return sqrt(X_MS);
}

std::tuple<Tensor, Tensor> LarsMomentum(  //
    const Tensor& X,                      //
    const Tensor& Grad,                   //
    const Tensor& Veloc,                  //
    const Tensor& LR,                     //
    double lars_coeff,                    //
    double lars_weight_decay,             //
    double momentum) {
  auto XNorm = Normalize(X);
  auto GradNorm = Normalize(Grad);
  auto LocLR = LR * lars_coeff * XNorm / (GradNorm + lars_weight_decay * XNorm);
  auto NewVeloc = momentum * Veloc + LocLR * (Grad + lars_weight_decay * X);
  return std::make_tuple(X - NewVeloc, NewVeloc);
}

TEST_F(CppEdsl, LarsMomentum4d) {
  auto X_shape = LogicalShape(DType::FLOAT32, {4, 7, 3, 9});
  auto LR_shape = LogicalShape(DType::FLOAT32, {});
  auto X = Placeholder(X_shape);
  auto Grad = Placeholder(X_shape);
  auto Veloc = Placeholder(X_shape);
  auto LR = Placeholder(LR_shape);
  auto R = LarsMomentum(X, Grad, Veloc, LR, 1. / 1024., 1. / 2048., 1. / 8.);
  Program program("lars_momentum4d", {std::get<0>(R), std::get<1>(R)});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<() -> ()>
#map1 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>


!f32 = type tensor<!eltwise.f32>
module {
  func @lars_momentum4d(%arg0: tensor<4x7x3x9x!eltwise.f32>, %arg1: tensor<4x7x3x9x!eltwise.f32>, %arg2: !f32, %arg3: tensor<4x7x3x9x!eltwise.f32>) -> (tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>) {
    %cst = "eltwise.sconst"() {value = 1.250000e-01 : f64} : () -> !f32
    %cst_0 = "eltwise.sconst"() {value = 9.765625E-4 : f64} : () -> !f32
    %cst_1 = "eltwise.sconst"() {value = 4.8828125E-4 : f64} : () -> !f32
    %cst_2 = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = "eltwise.mul"(%arg0, %cst_1) : (tensor<4x7x3x9x!eltwise.f32>, !f32) -> tensor<4x7x3x9x!eltwise.f32>
    %1 = "eltwise.add"(%arg1, %0) : (tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>) -> tensor<4x7x3x9x!eltwise.f32>
    %2 = "eltwise.mul"(%arg0, %arg0) : (tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>) -> tensor<4x7x3x9x!eltwise.f32>
    %3 = tile.cion add, none, %cst_2, %2 {sink = #map0, srcs = [#map1]} : !f32, tensor<4x7x3x9x!eltwise.f32> -> !f32
    %4 = "eltwise.sqrt"(%3) : (!f32) -> !f32
    %5 = "eltwise.mul"(%4, %cst_1) : (!f32, !f32) -> !f32
    %6 = "eltwise.mul"(%arg1, %arg1) : (tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>) -> tensor<4x7x3x9x!eltwise.f32>
    %7 = tile.cion add, none, %cst_2, %6 {sink = #map0, srcs = [#map1]} : !f32, tensor<4x7x3x9x!eltwise.f32> -> !f32
    %8 = "eltwise.sqrt"(%7) : (!f32) -> !f32
    %9 = "eltwise.add"(%8, %5) : (!f32, !f32) -> !f32
    %10 = "eltwise.mul"(%arg2, %cst_0) : (!f32, !f32) -> !f32
    %11 = "eltwise.mul"(%10, %4) : (!f32, !f32) -> !f32
    %12 = "eltwise.div"(%11, %9) : (!f32, !f32) -> !f32
    %13 = "eltwise.mul"(%12, %1) : (!f32, tensor<4x7x3x9x!eltwise.f32>) -> tensor<4x7x3x9x!eltwise.f32>
    %14 = "eltwise.mul"(%arg3, %cst) : (tensor<4x7x3x9x!eltwise.f32>, !f32) -> tensor<4x7x3x9x!eltwise.f32>
    %15 = "eltwise.add"(%14, %13) : (tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>) -> tensor<4x7x3x9x!eltwise.f32>
    %16 = "eltwise.sub"(%arg0, %15) : (tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>) -> tensor<4x7x3x9x!eltwise.f32>
    return %16, %15 : tensor<4x7x3x9x!eltwise.f32>, tensor<4x7x3x9x!eltwise.f32>
  }
}
)#"));
  // TODO: add 'sqrt' to std and llvm dialects
  // runProgram(program);
}

TEST_F(CppEdsl, RepeatElements) {
  auto I = Placeholder(DType::FLOAT32, {10, 10, 10});
  TensorDim N0, N1, N2;
  TensorIndex n0, n1, n2, k;
  I.bind_dims(N0, N1, N2);
  auto O = TensorOutput(N0, 3 * N1, N2);
  O(n0, 3 * n1 + k, n2) = I(n0, n1, n2);
  O.add_constraint(k < 3);
  O.no_reduce();
  Program program("repeat_elts", {O});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2, d3) -> (d0, d1 * 3 + d2, d3)>
#map1 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d3)>

#set0 = affine_set<(d0, d1, d2, d3) : (d2 >= 0, -d2 + 2 >= 0)>

!f32 = type tensor<!eltwise.f32>
module {
  func @repeat_elts(%arg0: tensor<10x10x10x!eltwise.f32>) -> tensor<10x30x10x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion assign, none, %cst, %arg0 {cons = #set0, no_reduce, sink = #map0, srcs = [#map1]} : !f32, tensor<10x10x10x!eltwise.f32> -> tensor<10x30x10x!eltwise.f32>
    return %0 : tensor<10x30x10x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, UseDefault) {
  auto P = Placeholder(DType::FLOAT32, {1, 7, 10, 10});
  auto I = Placeholder(DType::FLOAT32, {1, 10, 10});
  TensorDim B, N1, N2;
  TensorIndex b, i1, i2;
  I.bind_dims(B, N1, N2);
  auto O = TensorOutput(B, 7, N1, N2);
  O(b, 3, i1, i2) = I(b, i1, i2);
  O.use_default(P);
  Program program("use_default", {O});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2) -> (d0, 3, d1, d2)>
#map1 = affine_map<(d0, d1, d2) -> (d0, d1, d2)>


module {
  func @use_default(%arg0: tensor<1x10x10x!eltwise.f32>, %arg1: tensor<1x7x10x10x!eltwise.f32>) -> tensor<1x7x10x10x!eltwise.f32> {
    %0 = tile.cion assign, none, %arg1, %arg0 {sink = #map0, srcs = [#map1]} : tensor<1x7x10x10x!eltwise.f32>, tensor<1x10x10x!eltwise.f32> -> tensor<1x7x10x10x!eltwise.f32>
    return %0 : tensor<1x7x10x10x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

Tensor ArgMax(const Tensor& I) {
  TensorDim X0, X1, X2;
  TensorIndex x0, x1, x2;
  I.bind_dims(X0, X1, X2);
  auto Max = TensorOutput(X0, X2);
  Max(x0, x2) >= I(x0, x1, x2);
  Tensor One{1};
  auto T = TensorOutput(X1);
  T(x1) = One();
  Tensor IX = index(T, 0);
  auto O = TensorOutput(X0, X2);
  O(x0, x2) >= cond(I(x0, x1, x2), Max(x0, x2), IX(x1));
  return cast(O, DType::UINT32);
}

TEST_F(CppEdsl, ArgMax) {
  auto I = Placeholder(DType::FLOAT32, {1, 10, 10});
  auto X = ArgMax(I);
  Program program("arg_max", {X});
  EXPECT_THAT(X.compute_shape(), Eq(LogicalShape(DType::UINT32, {1, 10})));
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0) -> (d0)>
#map1 = affine_map<() -> ()>
#map2 = affine_map<(d0, d1, d2) -> (d0, d1)>
#map3 = affine_map<(d0, d1, d2) -> (d0, d2, d1)>
#map4 = affine_map<(d0, d1, d2) -> (d2)>


!f32 = type tensor<!eltwise.f32>
!i32 = type tensor<!eltwise.i32>
module {
  func @arg_max(%arg0: tensor<1x10x10x!eltwise.f32>) -> tensor<1x10x!eltwise.u32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %c1 = "eltwise.sconst"() {value = 1 : i64} : () -> !i32
    %0 = tile.cion assign, none, %cst, %c1 {sink = #map0, srcs = [#map1]} : !f32, !i32 -> tensor<10x!eltwise.i32>
    %1 = "tile.index"(%0) {dim = 0 : i64} : (tensor<10x!eltwise.i32>) -> tensor<10x!eltwise.i32>
    %2 = tile.cion max, none, %cst, %arg0 {sink = #map2, srcs = [#map3]} : !f32, tensor<1x10x10x!eltwise.f32> -> tensor<1x10x!eltwise.f32>
    %3 = tile.cion max, cond, %cst, %arg0, %2, %1 {sink = #map2, srcs = [#map3, #map2, #map4]} : !f32, tensor<1x10x10x!eltwise.f32>, tensor<1x10x!eltwise.f32>, tensor<10x!eltwise.i32> -> tensor<1x10x!eltwise.i32>
    %4 = "eltwise.cast"(%3) : (tensor<1x10x!eltwise.i32>) -> tensor<1x10x!eltwise.u32>
    return %4 : tensor<1x10x!eltwise.u32>
  }
}
)#"));
  runProgram(program);
}

Tensor Winograd(const Tensor& I, const Tensor& K, const Tensor& A, const Tensor& B, const Tensor& G) {
  TensorDim N, S, X, Y, CI, CO, BI, BO;
  I.bind_dims(N, X, Y, CI);
  K.bind_dims(S, S, CI, CO);
  A.bind_dims(BI, BO);
  B.bind_dims(BI, BI);
  G.bind_dims(BI, S);
  auto XO = (X - S + 1) / 1;
  auto YO = (Y - S + 1) / 1;
  auto XB = (XO + BO - 1) / BO;
  auto YB = (YO + BO - 1) / BO;
  auto XP = 0, YP = 0;
  // assert(BI - CI + 1 == BO);
  auto U1 = TensorOutput(BI, S, CI, CO);
  auto U = TensorOutput(BI, BI, CI, CO);
  auto V1 = TensorOutput(N, BI, BI, XB, YB, CI);
  auto V = TensorOutput(N, BI, BI, XB, YB, CI);
  auto M = TensorOutput(N, BI, BI, XB, YB, CO);
  auto O1 = TensorOutput(N, BO, BI, XB, YB, CO);
  auto O = TensorOutput(N, XO, YO, CO);
  TensorIndex n, i, j, k, x, y, ci, co;
  U1(i, j, ci, co) += G(i, k) * K(k, j, ci, co);
  U(i, j, ci, co) += U1(i, k, ci, co) * G(j, k);
  V1(n, i, j, x, y, ci) += B(k, i) * I(n, BO * x + k - XP, BO * y + j - YP, ci);
  V(n, i, j, x, y, ci) += V1(n, i, k, x, y, ci) * B(k, j);
  M(n, i, j, x, y, co) += V(n, i, j, x, y, ci) * U(i, j, ci, co);
  O1(n, i, j, x, y, co) += A(k, i) * M(n, k, j, x, y, co);
  O(n, BO * x + i, BO * y + j, co) += O1(n, i, k, x, y, co) * A(k, j);
  O.no_reduce();
  return O;
}

TEST_F(CppEdsl, Winograd) {
  const std::int64_t N = 1, X = 224, Y = 224, CI = 3, S = 3, CO = 32, BI = 32, BO = BI - CI + 1;
  auto I = Placeholder(DType::FLOAT32, {N, X, Y, CI});
  auto K = Placeholder(DType::FLOAT32, {S, S, CI, CO});
  auto A = Placeholder(DType::FLOAT32, {BI, BO});
  auto B = Placeholder(DType::FLOAT32, {BI, BI});
  auto G = Placeholder(DType::FLOAT32, {BI, S});
  auto W = Winograd(I, K, A, B, G);
  Program program("winograd", {W});
  runProgram(program);
}

TEST_F(CppEdsl, UniqueNames) {
  LogicalShape shape(DType::FLOAT32, {1});
  auto A = Placeholder(shape, "A");
  auto B = Placeholder(shape, "B");
  auto C0 = Placeholder(shape, "C");
  auto C1 = Placeholder(shape, "C");
  Program program("unique_names", {A + B + C0 + C1});
  EXPECT_THAT(program, Eq(R"#(
module {
  func @unique_names(%arg0: tensor<1x!eltwise.f32> {tile.name = "C"}, %arg1: tensor<1x!eltwise.f32> {tile.name = "C_0"}, %arg2: tensor<1x!eltwise.f32> {tile.name = "B"}, %arg3: tensor<1x!eltwise.f32> {tile.name = "A"}) -> tensor<1x!eltwise.f32> {
    %0 = "eltwise.add"(%arg3, %arg2) : (tensor<1x!eltwise.f32>, tensor<1x!eltwise.f32>) -> tensor<1x!eltwise.f32>
    %1 = "eltwise.add"(%0, %arg1) : (tensor<1x!eltwise.f32>, tensor<1x!eltwise.f32>) -> tensor<1x!eltwise.f32>
    %2 = "eltwise.add"(%1, %arg0) : (tensor<1x!eltwise.f32>, tensor<1x!eltwise.f32>) -> tensor<1x!eltwise.f32>
    return %2 : tensor<1x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, GlobalMin) {
  auto I = Placeholder(DType::FLOAT32, {10, 10, 10}, "I");
  TensorIndex i, j, k;
  auto O_Neg = TensorOutput();
  auto Neg = -I;
  O_Neg() >= Neg(i, j, k);
  auto O = -O_Neg;
  Program program("global_min", {O});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<() -> ()>
#map1 = affine_map<(d0, d1, d2) -> (d0, d1, d2)>


!f32 = type tensor<!eltwise.f32>
module {
  func @global_min(%arg0: tensor<10x10x10x!eltwise.f32> {tile.name = "I"}) -> !f32 {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = "eltwise.neg"(%arg0) : (tensor<10x10x10x!eltwise.f32>) -> tensor<10x10x10x!eltwise.f32>
    %1 = tile.cion max, none, %cst, %0 {sink = #map0, srcs = [#map1]} : !f32, tensor<10x10x10x!eltwise.f32> -> !f32
    %2 = "eltwise.neg"(%1) : (!f32) -> !f32
    return %2 : !f32
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, CumSum) {
  auto I = Placeholder(DType::FLOAT32, {10}, "I");
  TensorDim N;
  TensorIndex i, k;
  I.bind_dims(N);
  auto O = TensorOutput(N);
  O(i) += I(k);
  O.add_constraint(i - k < N);
  Program program("cumsum", {O});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1) -> (d0)>
#map1 = affine_map<(d0, d1) -> (d1)>

#set0 = affine_set<(d0, d1) : (d0 - d1 >= 0, -d0 + d1 + 9 >= 0)>

!f32 = type tensor<!eltwise.f32>
module {
  func @cumsum(%arg0: tensor<10x!eltwise.f32> {tile.name = "I"}) -> tensor<10x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, none, %cst, %arg0 {cons = #set0, sink = #map0, srcs = [#map1]} : !f32, tensor<10x!eltwise.f32> -> tensor<10x!eltwise.f32>
    return %0 : tensor<10x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

Tensor ComplexConv2d(              //
    const Tensor& I,               //
    const Tensor& K,               //
    const std::vector<size_t>& s,  // stride coeffs
    const std::vector<size_t>& d   // dilation coeffs
) {
  // "same-lower" autopadding will be applied
  TensorDim N, G, GCI, GCO;
  std::vector<TensorDim> X(2);
  std::vector<TensorDim> KX(2);
  TensorIndex n, g, gci, gco;
  std::vector<TensorIndex> x(2);
  std::vector<TensorIndex> k(2);
  I.bind_dims(N, X[0], X[1], G, GCI);
  K.bind_dims(KX[0], KX[1], G, GCI, GCO);
  // Compute output spatial dimensions
  std::vector<TensorDim> Y(2);
  for (size_t i = 0; i < Y.size(); ++i) {
    Y[i] = (X[i] + s[i] - 1) / s[i];
  }
  // Compute the effective kernel size after dilation
  std::vector<TensorDim> EK(2);
  for (size_t i = 0; i < EK.size(); ++i) {
    EK[i] = d[i] * (KX[i] - 1) + 1;
  }
  // Compute the padding offset
  std::vector<TensorDim> P(2);
  for (size_t i = 0; i < P.size(); ++i) {
    P[i] = ((Y[i] - 1) * s[i] + EK[i] - X[i]) / 2;
  }
  // Specify the output size
  auto O = TensorOutput(N, Y[0], Y[1], G, GCO);
  // Compute the convolution
  O(n, x[0], x[1], g, gco) +=
      I(n, s[0] * x[0] + d[0] * k[0] - P[0], s[1] * x[1] + d[1] * k[1] - P[1], g, gci) * K(k[0], k[1], g, gci, gco);
  return O;
}

TEST_F(CppEdsl, ComplexConv2d) {
  auto I = Placeholder(DType::FLOAT32, {1, 224, 224, 3, 3});
  auto K = Placeholder(DType::FLOAT32, {3, 3, 3, 3, 32});
  auto O = ComplexConv2d(I, K, {2, 2}, {3, 3});
  Program program("complex_conv_2d", {O});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2, d3, d4, d5, d6, d7) -> (d0, d1, d2, d3, d4)>
#map1 = affine_map<(d0, d1, d2, d3, d4, d5, d6, d7) -> (d0, d1 * 2 + d5 * 3 - 2, d2 * 2 + d6 * 3 - 2, d3, d7)>
#map2 = affine_map<(d0, d1, d2, d3, d4, d5, d6, d7) -> (d5, d6, d3, d7, d4)>


!f32 = type tensor<!eltwise.f32>
module {
  func @complex_conv_2d(%arg0: tensor<3x3x3x3x32x!eltwise.f32>, %arg1: tensor<1x224x224x3x3x!eltwise.f32>) -> tensor<1x112x112x3x32x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg1, %arg0 {sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x224x224x3x3x!eltwise.f32>, tensor<3x3x3x3x32x!eltwise.f32> -> tensor<1x112x112x3x32x!eltwise.f32>
    return %0 : tensor<1x112x112x3x32x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, Reciprocal) {
  auto A = Placeholder(DType::FLOAT32, {6}, "A");
  auto R = 1.0 / A;
  Program program("reciprocal", {R});
  EXPECT_THAT(program, Eq(R"#(
!f32 = type tensor<!eltwise.f32>
module {
  func @reciprocal(%arg0: tensor<6x!eltwise.f32> {tile.name = "A"}) -> tensor<6x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 1.000000e+00 : f64} : () -> !f32
    %0 = "eltwise.div"(%cst, %arg0) : (!f32, tensor<6x!eltwise.f32>) -> tensor<6x!eltwise.f32>
    return %0 : tensor<6x!eltwise.f32>
  }
}
)#"));
  std::vector<float> input = {1, 2, 4, 5, 8, 10};
  std::vector<float> expected = {1.0, 0.5, 0.25, 0.2, 0.125, 0.1};
  checkProgram(program, {{A, input}}, {{R, expected}});
}

// TEST_F(CppEdsl, GradientDot) {
//   auto A = Placeholder(DType::FLOAT32, {100, 100}, "A");
//   auto B = Placeholder(DType::FLOAT32, {100, 100}, "B");
//   auto O = Dot(A, B);
//   auto grads = Gradient({A, B}, O);
//   Program program("gradient_dot", {grads});
//   EXPECT_THAT(program, Eq(R"(function (
//   A[A_0, A_1],
//   B[B_0, B_1]
// ) -> (
//   _X3,
//   _X2
// ) {
//   _X0 = 1.000000;
//   _X1[x0, x1 : 100, 100] = +(_X0[]);
//   _X2[k, j : 100, 100] = +(A[i, k] * _X1[i, j]);
//   _X3[i, k : 100, 100] = +(_X1[i, j] * B[k, j]);
// }
// )"));
//   runProgram(program);
// }

// Tensor Max2Da0(const Tensor& A) {
//   TensorDim M, N;
//   A.bind_dims(M, N);
//   TensorIndex m("m"), n("n");
//   auto O = NamedTensorOutput("O", N);
//   O(n) >= A(m, n);
//   // O(n) += A(m, n);
//   return O;
// }

// TEST_F(CppEdsl, GradientMultiDot) {
//   auto A = Placeholder(DType::FLOAT32, {100, 100}, "A");
//   auto B = Placeholder(DType::FLOAT32, {100, 100}, "B");
//   auto C = Dot(A, B);
//   auto D = Dot(A, C);
//   auto O = Max2Da0(D);
//   auto grads = Gradient({A, B}, O);
//   Program program("gradient_dot", {grads});
//   EXPECT_THAT(program, Eq(R"(function (
//   A[A_0, A_1],
//   B[B_0, B_1]
// ) -> (
//   _X9,
//   _X6
// ) {
//   _X0[i, j : 100, 100] = +(A[i, k] * B[k, j]);
//   _X1[i, j : 100, 100] = +(A[i, k] * _X0[k, j]);
//   O[n : 100] = >(_X1[m, n]);
//   _X2 = 1.000000;
//   _X3[x0 : 100] = +(_X2[]);
//   _X4[m, n : 100, 100] = +(_X1[m, n] == O[n] ? _X3[n]);
//   _X5[k, j : 100, 100] = +(A[i, k] * _X4[i, j]);
//   _X6[k, j : 100, 100] = +(A[i, k] * _X5[i, j]);
//   _X7[i, k : 100, 100] = +(_X4[i, j] * _X0[k, j]);
//   _X8[i, k : 100, 100] = +(_X5[i, j] * B[k, j]);
//   _X9 = add(_X7, _X8);
// }
// )"));
//   runProgram(program);
// }

// TEST_F(CppEdsl, GradientDotSqrt) {
//   auto A = Placeholder(DType::FLOAT32, {100, 100}, "A");
//   auto B = Placeholder(DType::FLOAT32, {100, 100}, "B");
//   auto C = Dot(A, B);
//   auto O = sqrt(C);
//   auto grads = Gradient({A, B}, O);
//   Program program("gradient_dot", {grads});
//   EXPECT_THAT(program, Eq(R"(function (
//   A[A_0, A_1],
//   B[B_0, B_1]
// ) -> (
//   _X8,
//   _X7
// ) {
//   _X0 = 1.000000;
//   _X1[x0, x1 : 100, 100] = +(_X0[]);
//   _X2 = 2;
//   _X3[i, j : 100, 100] = +(A[i, k] * B[k, j]);
//   _X4 = sqrt(_X3);
//   _X5 = mul(_X2, _X4);
//   _X6 = div(_X1, _X5);
//   _X7[k, j : 100, 100] = +(A[i, k] * _X6[i, j]);
//   _X8[i, k : 100, 100] = +(_X6[i, j] * B[k, j]);
// }
// )"));
//   runProgram(program);
// }

TEST_F(CppEdsl, DefractLong) {
  std::vector<int64_t> input_shape{1, 3, 3, 1};
  std::vector<int64_t> output_shape{1, 5, 5, 1};
  auto I = Placeholder(DType::FLOAT32, input_shape, "I");
  auto K = Placeholder(DType::FLOAT32, input_shape, "K");
  auto O = TensorOutput(output_shape);
  TensorIndex n, x0, x1, k0, k1, co, ci;
  O(n, x0, x1, co) += I(n, (x0 + k0 - 1) / 2, (x1 + k1 - 1) / 2, ci) * K(2 - k0, 2 - k1, co, ci);
  Program program("defract_long", {O});
  EXPECT_THAT(program, Eq(R"#(
#map0 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d0, d1, d2, d3)>
#map1 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (d0, (d1 + d4 - 1) floordiv 2, (d2 + d5 - 1) floordiv 2, d6)>
#map2 = affine_map<(d0, d1, d2, d3, d4, d5, d6) -> (-d4 + 2, -d5 + 2, d3, d6)>


!f32 = type tensor<!eltwise.f32>
module {
  func @defract_long(%arg0: tensor<1x3x3x1x!eltwise.f32> {tile.name = "K"}, %arg1: tensor<1x3x3x1x!eltwise.f32> {tile.name = "I"}) -> tensor<1x5x5x1x!eltwise.f32> {
    %cst = "eltwise.sconst"() {value = 0.000000e+00 : f64} : () -> !f32
    %0 = tile.cion add, mul, %cst, %arg1, %arg0 {sink = #map0, srcs = [#map1, #map2]} : !f32, tensor<1x3x3x1x!eltwise.f32>, tensor<1x3x3x1x!eltwise.f32> -> tensor<1x5x5x1x!eltwise.f32>
    return %0 : tensor<1x5x5x1x!eltwise.f32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, DupOut) {
  auto A = Placeholder(DType::FLOAT32, {10, 20});
  auto B = Placeholder(DType::FLOAT32, {20, 30});
  auto C = Placeholder(DType::FLOAT32, {30, 40});
  auto R = Dot(Dot(A, B), C);
  Program program("dup_out", {R, R, R});
  runProgram(program);
}

TEST_F(CppEdsl, Select) {
  auto I = Placeholder(DType::FLOAT32, {10, 20});
  auto O = select(I == 0, Tensor{0}, Tensor{1});
  Program program("select", {O});
  EXPECT_THAT(program, Eq(R"#(
!i32 = type tensor<!eltwise.i32>
module {
  func @select(%arg0: tensor<10x20x!eltwise.f32>) -> tensor<10x20x!eltwise.i32> {
    %c0 = "eltwise.sconst"() {value = 0 : i64} : () -> !i32
    %c1 = "eltwise.sconst"() {value = 1 : i64} : () -> !i32
    %0 = "eltwise.cmp_eq"(%arg0, %c0) : (tensor<10x20x!eltwise.f32>, !i32) -> tensor<10x20x!eltwise.u1>
    %1 = "eltwise.select"(%0, %c0, %c1) : (tensor<10x20x!eltwise.u1>, !i32, !i32) -> tensor<10x20x!eltwise.i32>
    return %1 : tensor<10x20x!eltwise.i32>
  }
}
)#"));
  runProgram(program);
}

TEST_F(CppEdsl, Shape) {
  auto I = Placeholder(DType::FLOAT32, {10, 20});
  auto O = shape(I);
  Program program("shape", {O});
  EXPECT_THAT(program, Eq(R"#(
module {
  func @shape(%arg0: tensor<10x20x!eltwise.f32>) -> tensor<2x!eltwise.i32> {
    %0 = "tile.shape"(%arg0) : (tensor<10x20x!eltwise.f32>) -> tensor<2x!eltwise.i32>
    return %0 : tensor<2x!eltwise.i32>
  }
}
)#"));
  exec::Binder binder(program);
  binder.compile()->run();
  IVLOG(1, "output: " << O.as_ptr());
  auto view = binder.output(O).mmap_current();
  auto data = reinterpret_cast<const int32_t*>(view.data());
  ASSERT_THAT(view.size(), sizeof(int32_t) * 2);
  EXPECT_THAT(data[0], 10);
  EXPECT_THAT(data[1], 20);
}

TEST_F(CppEdsl, Prng) {
  auto S = Placeholder(DType::UINT32, {3, 2048});
  auto O = prng(S, {2, 3, 4, 5});
  Program program("prng", {O});
  EXPECT_THAT(program, Eq(R"#(
!i32 = type tensor<!eltwise.i32>
module {
  func @prng(%arg0: tensor<3x2048x!eltwise.u32>) -> (tensor<2x3x4x5x!eltwise.f32>, tensor<3x2048x!eltwise.u32>) {
    %c2 = "eltwise.sconst"() {value = 2 : i64} : () -> !i32
    %c3 = "eltwise.sconst"() {value = 3 : i64} : () -> !i32
    %c4 = "eltwise.sconst"() {value = 4 : i64} : () -> !i32
    %c5 = "eltwise.sconst"() {value = 5 : i64} : () -> !i32
    %result, %new_state = "tile.prng"(%arg0, %c2, %c3, %c4, %c5) : (tensor<3x2048x!eltwise.u32>, !i32, !i32, !i32, !i32) -> (tensor<2x3x4x5x!eltwise.f32>, tensor<3x2048x!eltwise.u32>)
    return %result, %new_state : tensor<2x3x4x5x!eltwise.f32>, tensor<3x2048x!eltwise.u32>
  }
}
)#"));
  // TODO: lowering for PrngOp
  // runProgram(program);
}

Tensor add_poly_poly_coeffmod_2d(const Tensor& cipher, const Tensor& plain, const Tensor& coeff_modulus) {
  TensorDim L;  // coeff_mod_count
  TensorDim N;  // poly_modulus_degree

  cipher.bind_dims(L, N);
  plain.bind_dims(L, N);
  coeff_modulus.bind_dims(L, 1);

  auto sum = TensorOutput(L, N);
  sum = cipher + plain;
  // return sum;

  auto cmp = cast(cast(sum >= coeff_modulus, DType::INT64), DType::UINT64);
  auto P = cmp * coeff_modulus;
  auto R_sum = sum - P;
  return R_sum;

  /* auto cmp = cast(-cast(sum >= coeff_modulus, plaidml::DType::INT64),
  plaidml::DType::UINT64); sum = sum - (coeff_modulus & cmp); return sum; */
}

TEST_F(CppEdsl, AddPlain) {
  long int N = 8192;
  long int L = 3;
  auto cipher_in = Placeholder(DType::UINT64, {L, N});
  auto plain_in = Placeholder(DType::UINT64, {L, N});
  auto q = Placeholder(DType::UINT64, {L, 1});

  auto cipher_out = add_poly_poly_coeffmod_2d(cipher_in, plain_in, q);

  Program program("add_plain", {cipher_out});
  IVLOG(1, "program " << program);
  auto binder = exec::Binder(program);
  auto executable = binder.compile();

  std::vector<std::uint64_t> cipher_data(N * L);
  std::vector<std::uint64_t> plain_data(N * L);

  for (long int i = 0; i < N * L; ++i) {
    cipher_data[i] = i + 1;
    plain_data[i] = i + 37;
  }

  std::vector<std::uint64_t> coeff_mods{10, 20, 30};

  binder.input(cipher_in).copy_from(cipher_data.data());
  binder.input(plain_in).copy_from(plain_data.data());
  binder.input(q).copy_from(coeff_mods.data());

  auto t0 = std::chrono::system_clock::now();
  executable->run();

  auto trials = 100;
  auto t1 = std::chrono::system_clock::now();
  for (auto i = 0; i < trials; ++i) {
    executable->run();
  }
  auto t2 = std::chrono::system_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / static_cast<float>(trials);
  std::cout << "plaid add_plain_inplace time " << time << " us" << std::endl;

  auto time_first = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  std::cout << "plaid add_plain_inplace time_first " << time_first << " us" << std::endl;
}

plaidml::edsl::Tensor dyadic_product_coeffmod_3d(const plaidml::edsl::Tensor& Poly1, const plaidml::edsl::Tensor& Poly2,
                                                 const plaidml::edsl::Tensor& Qs, const plaidml::edsl::Tensor& CRs_0,
                                                 const plaidml::edsl::Tensor& CRs_1) {
  plaidml::edsl::TensorDim S;  // size
  plaidml::edsl::TensorDim L;  // coeff_mod_count
  plaidml::edsl::TensorDim N;  // poly_modulus_degree

  Poly1.bind_dims(S, L, N);
  Poly2.bind_dims(1, L, N);
  Qs.bind_dims(1, L, 1);
  CRs_0.bind_dims(1, L, 1);
  CRs_1.bind_dims(1, L, 1);

  // [Z_hi, Z_lo] contains [64-bit, 64-bit] product of poly1 * poly2;
  auto I1_lo_1 = cast(Poly1, plaidml::DType::UINT32);
  auto I1_hi_1 = cast(Poly1 >> 32, plaidml::DType::UINT32);
  auto I2_lo_1 = cast(Poly2, plaidml::DType::UINT32);
  auto I2_hi_1 = cast(Poly2 >> 32, plaidml::DType::UINT32);
  auto P11_1 = cast(cast(I1_hi_1, plaidml::DType::UINT64) * I2_hi_1, plaidml::DType::UINT64);
  auto P01_1 = cast(cast(I1_lo_1, plaidml::DType::UINT64) * I2_hi_1, plaidml::DType::UINT64);
  auto P10_1 = cast(cast(I1_hi_1, plaidml::DType::UINT64) * I2_lo_1, plaidml::DType::UINT64);
  auto P00_1 = cast(cast(I1_lo_1, plaidml::DType::UINT64) * I2_lo_1, plaidml::DType::UINT64);
  auto P10_1_cast = cast(cast(P10_1, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P01_1_cast = cast(cast(P01_1, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P00_1_shift = (P00_1 >> 32);
  auto C_sum_1 = (P10_1_cast + P01_1_cast + P00_1_shift);
  auto Carry_1 = cast(C_sum_1 >> 32, plaidml::DType::UINT64);
  auto Z_hi = P11_1 + cast(P10_1 >> 32, plaidml::DType::UINT64) + cast(P01_1 >> 32, plaidml::DType::UINT64) + Carry_1;
  auto Z_lo = Poly1 * Poly2;

  auto I1_lo_2 = cast(Z_lo, plaidml::DType::UINT32);
  auto I1_hi_2 = cast(Z_lo >> 32, plaidml::DType::UINT32);
  auto I2_lo_2 = cast(CRs_0, plaidml::DType::UINT32);
  auto I2_hi_2 = cast(CRs_0 >> 32, plaidml::DType::UINT32);
  auto I1_hi_2_u64 = cast(I1_hi_2, plaidml::DType::UINT64);
  auto I1_lo_2_u64 = cast(I1_lo_2, plaidml::DType::UINT64);
  auto P11_2_u64 = cast(I1_hi_2_u64 * I2_hi_2, plaidml::DType::UINT64);
  auto P01_2_u64 = cast(I1_lo_2_u64 * I2_hi_2, plaidml::DType::UINT64);
  auto P10_2_u64 = cast(I1_hi_2_u64 * I2_lo_2, plaidml::DType::UINT64);
  auto P00_2_u64 = cast(I1_lo_2_u64 * I2_lo_2, plaidml::DType::UINT64);
  auto P10_2_cast = cast(cast(P10_2_u64, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P01_2_cast = cast(cast(P01_2_u64, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P00_2_shift = (P00_2_u64 >> 32);
  auto C_sum_2 = (P10_2_cast + P01_2_cast + P00_2_shift);
  auto Carry_2 = cast(C_sum_2 >> 32, plaidml::DType::UINT64);
  auto Dyadic_carry = P11_2_u64 + cast(P10_2_u64 >> 32, plaidml::DType::UINT64) +
                      cast(P01_2_u64 >> 32, plaidml::DType::UINT64) + Carry_2;

  auto I1_lo_3 = cast(Z_lo, plaidml::DType::UINT32);
  auto I1_hi_3 = cast(Z_lo >> 32, plaidml::DType::UINT32);
  auto I2_lo_3 = cast(CRs_1, plaidml::DType::UINT32);
  auto I2_hi_3 = cast(CRs_1 >> 32, plaidml::DType::UINT32);
  auto I1_hi_3_u64 = cast(I1_hi_3, plaidml::DType::UINT64);
  auto I1_lo_3_u64 = cast(I1_lo_3, plaidml::DType::UINT64);
  auto P11_3_u64 = cast(I1_hi_3_u64 * I2_hi_3, plaidml::DType::UINT64);
  auto P01_3_u64 = cast(I1_lo_3_u64 * I2_hi_3, plaidml::DType::UINT64);
  auto P10_3_u64 = cast(I1_hi_3_u64 * I2_lo_3, plaidml::DType::UINT64);
  auto P00_3_u64 = cast(I1_lo_3_u64 * I2_lo_3, plaidml::DType::UINT64);
  auto P10_3_cast = cast(cast(P10_3_u64, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P01_3_cast = cast(cast(P01_3_u64, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P00_3_shift = (P00_3_u64 >> 32);
  auto C_sum_3 = (P10_3_cast + P01_3_cast + P00_3_shift);
  auto Carry_3 = cast(C_sum_3 >> 32, plaidml::DType::UINT64);
  auto Tmp2_hi = P11_3_u64 + cast(P10_3_u64 >> 32, plaidml::DType::UINT64) +
                 cast(P01_3_u64 >> 32, plaidml::DType::UINT64) + Carry_3;
  auto Tmp2_lo = Z_lo * CRs_1;

  auto Tmp1 = Tmp2_lo + Dyadic_carry;

  auto C_add_uint_1 = Tmp1 < Tmp2_lo;
  auto C_add_uint_u8_1 = cast(C_add_uint_1, plaidml::DType::UINT8);
  auto Tmp3 = Tmp2_hi + C_add_uint_u8_1;

  auto I1_lo_4 = cast(Z_hi, plaidml::DType::UINT32);
  auto I1_hi_4 = cast(Z_hi >> 32, plaidml::DType::UINT32);
  auto I2_lo_4 = cast(CRs_0, plaidml::DType::UINT32);
  auto I2_hi_4 = cast(CRs_0 >> 32, plaidml::DType::UINT32);
  auto I1_hi_4_u64 = cast(I1_hi_4, plaidml::DType::UINT64);
  auto I1_lo_4_u64 = cast(I1_lo_4, plaidml::DType::UINT64);
  auto P11_4_u64 = cast(I1_hi_4_u64 * I2_hi_4, plaidml::DType::UINT64);
  auto P01_4_u64 = cast(I1_lo_4_u64 * I2_hi_4, plaidml::DType::UINT64);
  auto P10_4_u64 = cast(I1_hi_4_u64 * I2_lo_4, plaidml::DType::UINT64);
  auto P00_4_u64 = cast(I1_lo_4_u64 * I2_lo_4, plaidml::DType::UINT64);
  auto P10_4_cast = cast(cast(P10_4_u64, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P01_4_cast = cast(cast(P01_4_u64, plaidml::DType::UINT32), plaidml::DType::UINT64);
  auto P00_4_shift = (P00_4_u64 >> 32);
  auto C_sum_4 = (P10_4_cast + P01_4_cast + P00_4_shift);
  auto Carry_4 = cast(C_sum_4 >> 32, plaidml::DType::UINT64);
  auto Tmp2_hi_2 = P11_4_u64 + cast(P10_4_u64 >> 32, plaidml::DType::UINT64) +
                   cast(P01_4_u64 >> 32, plaidml::DType::UINT64) + Carry_4;
  auto Tmp2_lo_2 = Z_hi * CRs_0;

  auto S_add_uint_2 = Tmp1 + Tmp2_lo_2;
  auto C_add_uint_2 = S_add_uint_2 < Tmp1;
  auto Tmp1_2 = cast(C_add_uint_2, plaidml::DType::UINT8);
  auto Dyadic_carry_2 = Tmp2_hi_2 + Tmp1_2;

  // This is all we care about
  auto Tmp1_3 = (Z_hi * CRs_1) + Tmp3 + Dyadic_carry_2;

  // Barrett subtraction
  auto Tmp3_2 = Z_lo - (Tmp1_3 * Qs);

  // Claim: One more subtraction is enough
  auto cmp_final_cast = cast(Tmp3_2 >= Qs, plaidml::DType::INT64);
  auto cmp_final_sub = cmp_final_cast * Qs;
  auto R = Tmp3_2 - cmp_final_sub;

  // TODO(fboemer): use below instead once negi has been added to mlir standard
  // dialect
  /* auto R = Tmp3_2 - (cast(-cast(Tmp3_2 >= Qs, plaidml::DType::INT64),
                          plaidml::DType::UINT64) &
                     Qs); */
  return R;
}

TEST_F(CppEdsl, MultPlain) {
  long int N = 8192;
  long int L = 3;
  auto cipher_in = Placeholder(DType::UINT64, {2, L, N});
  auto plain_in = Placeholder(DType::UINT64, {1, L, N});
  auto q = Placeholder(DType::UINT64, {1, L, 1});
  auto cr0s = Placeholder(DType::UINT64, {1, L, 1});
  auto cr1s = Placeholder(DType::UINT64, {1, L, 1});

  auto cipher_out = dyadic_product_coeffmod_3d(cipher_in, plain_in, q, cr0s, cr1s);

  Program program("mult_plain", {cipher_out});
  IVLOG(1, "program " << program);
  auto binder = exec::Binder(program);
  auto executable = binder.compile();

  std::vector<std::uint64_t> cipher_data(N * L);
  std::vector<std::uint64_t> plain_data(N * L);

  for (long int i = 0; i < N * L; ++i) {
    cipher_data[i] = i + 1;
    plain_data[i] = i + 37;
  }

  std::vector<std::uint64_t> coeff_mods{10, 20, 30};

  binder.input(cipher_in).copy_from(cipher_data.data());
  binder.input(plain_in).copy_from(plain_data.data());
  binder.input(q).copy_from(coeff_mods.data());
  binder.input(cr0s).copy_from(coeff_mods.data());
  binder.input(cr1s).copy_from(coeff_mods.data());

  auto t0 = std::chrono::system_clock::now();
  executable->run();

  auto trials = 100;
  auto t1 = std::chrono::system_clock::now();
  for (auto i = 0; i < trials; ++i) {
    executable->run();
  }
  auto t2 = std::chrono::system_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / static_cast<float>(trials);
  std::cout << "plaid mult_plain_inplace time " << time << " us" << std::endl;

  auto time_first = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  std::cout << "plaid mult_plain_inplace time_first " << time_first << " us" << std::endl;
}

}  // namespace
}  // namespace plaidml::edsl
