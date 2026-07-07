#pragma once

#include <httplib.h>

#include "dashboard_server.h"

void register_routes(httplib::Server& server, DashboardServer& dashboard);
