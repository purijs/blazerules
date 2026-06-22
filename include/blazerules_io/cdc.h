#ifndef BLAZERULES_IO_CDC_H
#define BLAZERULES_IO_CDC_H

#include <string>
#include <string_view>
#include <vector>

namespace blazerules_io {

std::string unwrap_debezium(const std::vector<std::string_view>& messages,
                            const std::string& op_field = "__op");

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_CDC_H
