/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed in accordance with the terms specified in
 *  the LICENSE file found in the root directory of this source tree.
 */

#include <osquery/database.h>
#include <osquery/flags.h>
#include <osquery/logger.h>
#include <osquery/registry_factory.h>

#include <boost/variant.hpp>

namespace osquery {

DECLARE_string(database_path);

class EphemeralDatabasePlugin : public DatabasePlugin {
  using DBType =
      std::map<std::string,
               std::map<std::string, boost::variant<int, std::string>>>;
  template <typename T>
  Status getAny(const std::string& domain,
                const std::string& key,
                T& value) const;

 private:
  void setValue(const std::string& domain,
                const std::string& key,
                const std::string& value);

  void setValue(const std::string& domain, const std::string& key, int value);

 public:
  /// Data retrieval method.

  Status get(const std::string& domain,
             const std::string& key,
             std::string& value) const override;
  Status get(const std::string& domain,
             const std::string& key,
             int& value) const override;
  /// Data storage method.
  Status put(const std::string& domain,
             const std::string& key,
             const std::string& value) override;
  Status put(const std::string& domain,
             const std::string& key,
             int value) override;

  Status putBatch(const std::string& domain,
                  const DatabaseStringValueList& data) override;

  void dumpDatabase() const override;

  /// Data removal method.
  Status remove(const std::string& domain, const std::string& k) override;

  Status removeRange(const std::string& domain,
                     const std::string& low,
                     const std::string& high) override;

  /// Key/index lookup method.
  Status scan(const std::string& domain,
              std::vector<std::string>& results,
              const std::string& prefix,
              size_t max) const override;

 public:
  /// Database workflow: open and setup.
  Status setUp() override {
    DBType().swap(db_);
    return Status(0);
  }

 private:
  DBType db_;
};

/// Backing-storage provider for osquery internal/core.
REGISTER_INTERNAL(EphemeralDatabasePlugin, "database", "ephemeral");

} // namespace osquery
