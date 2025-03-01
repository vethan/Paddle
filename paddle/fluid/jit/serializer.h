// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>

#include "paddle/fluid/framework/var_desc.h"
#include "paddle/fluid/framework/variable.h"
#include "paddle/fluid/jit/property.h"

#include "paddle/fluid/jit/layer.h"

namespace paddle {
namespace jit {
// Export Layer into local disk
class Serializer {
 public:
  void operator()(const Layer& layer, const std::string& file_dir);

  //  private:
  //   void WriteTensorData(const Layer& layer, const std::string& file_name)
  //   const;
  //   void WriteExtraInfo(const Layer& layer, const std::string& file_name)
  //   const;
  //   void WriteByteCode(const Layer& layer, const std::string& file_name)
  //   const;
};

class Deserializer {
 public:
  Layer operator()(const std::string& dir_path, const phi::Place& place);

 private:
  void ReadTensorData(const std::string& file_name,
                      const std::set<std::string>& var_name,
                      const phi::Place& place,
                      Name2VariableMap* params_dict) const;

  // property pb
  void ReadAttributeData(const std::string& file_path,
                         Name2VariableMap* attrs_dict) const;

  // void ReadExtraInfo(const std::string& file_name) const;

  // void ReadByteCode(const std::string& file_name) const;

  framework::ProgramDesc LoadProgram(const std::string& file_name);
};

void Export(const Layer& layer, const std::string& file_path);

// path should be like 'dirname/file_prefix'
Layer Load(const std::string& path, const phi::Place& place);

}  // namespace jit
}  // namespace paddle
