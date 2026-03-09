//
//  cloudsync_duckdb.hpp
//  cloudsync
//
//  DuckDB extension entry point
//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class CloudsyncExtension : public Extension {
public:
    void Load(ExtensionLoader &loader) override;
    std::string Name() override;
    std::string Version() const override;
};

} // namespace duckdb
