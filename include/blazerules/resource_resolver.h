#ifndef BLAZERULES_RESOURCE_RESOLVER_H
#define BLAZERULES_RESOURCE_RESOLVER_H

#include <string>

namespace blazerules {

bool is_s3_uri(const std::string& uri);
std::string resource_parent(const std::string& uri);
std::string join_resource_path(const std::string& base, const std::string& path);
std::string resolve_resource_to_local(const std::string& uri);

void set_aws_profile(const std::string& profile, bool clear_env_credentials = true);
void clear_aws_profile();
std::string current_aws_profile();
void set_aws_region(const std::string& region);
void clear_aws_region();
std::string current_aws_region();
void set_aws_endpoint_url(const std::string& endpoint_url);
void clear_aws_endpoint_url();
std::string current_aws_endpoint_url();
void set_aws_credentials(const std::string& access_key_id,
                         const std::string& secret_access_key,
                         const std::string& session_token = "",
                         const std::string& region = "");
void clear_aws_credentials();

} // namespace blazerules

#endif // BLAZERULES_RESOURCE_RESOLVER_H
