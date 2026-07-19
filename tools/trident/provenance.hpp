#pragma once
#include "package.hpp"
#include "provider.hpp"
#include <string>
#include <vector>

struct AuditRecord {
    std::string path;
    std::string selected;
    std::string hash;
    std::string auditor;
};

struct TrustedKey {
    std::string identity;
    std::string publicKey;
};

struct AuditPolicy {
    std::vector<std::string> trustedAuditors;
    std::vector<std::string> auditFiles;
    bool requireAttestations = false;
    std::vector<std::string> attestationDirs;
    std::vector<TrustedKey> keys;
};

struct Attestation {
    std::string path;
    std::string selected;
    std::string hash;
    std::string commit;
    std::string identity;
    std::string artifact;
    std::string artifactHash;
    std::string signature;  // lowercase hex, over the canonical statement
};

bool readAuditRecords(const std::string& path, std::vector<AuditRecord>& records,
                      std::string& err);
bool appendAuditRecord(const std::string& path, const AuditRecord& record,
                       std::string& err);
bool readAuditPolicy(const std::string& path, AuditPolicy& policy, std::string& err);

// Enforce a project's opt-in policy over the already integrity-verified
// build list. Relative audit/key/attestation paths are resolved beside the
// policy file. A trusted-auditors list requires at least one matching audit
// per module; require_attestations additionally requires a valid signature
// from one configured [[key]] identity per module.
bool enforceAuditPolicy(const std::string& policyPath,
                        const std::vector<BuildListEntry>& buildList,
                        std::vector<std::string>& report, std::string& err);

// Create/read the signed source provenance used by the policy above. Signing
// and verification fork the system openssl CLI only when this opt-in feature
// is used; Trident itself gains no linked crypto dependency.
bool writeSignedAttestation(const PackageSnapshot& package, const std::string& identity,
                            const std::string& privateKey, const std::string& artifact,
                            const std::string& outputPath, std::string& err);
bool readAttestation(const std::string& path, Attestation& out, std::string& err);

std::string defaultAttestationPath(const PackageSnapshot& package,
                                   const std::string& identity);
