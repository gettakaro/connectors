#pragma once
// The release build passes the connector version (/DTAKARO_PLUGIN_VERSION="x.y.z"); this default is for local builds.
#ifndef TAKARO_PLUGIN_VERSION
#define TAKARO_PLUGIN_VERSION "0.1.0" // x-release-please-version
#endif
#define TAKARO_PLUGIN_VERSION_W L"" TAKARO_PLUGIN_VERSION
#define TAKARO_NATIVE_VERSION_STRING TAKARO_PLUGIN_VERSION
