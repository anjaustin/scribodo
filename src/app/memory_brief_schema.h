/* Parent registers this after migration 012. No consent is enabled here. */
#ifndef SCRIBODO_MEMORY_BRIEF_SCHEMA_H
#define SCRIBODO_MEMORY_BRIEF_SCHEMA_H

#include <stddef.h>

static const char *const k_migration_013[] = {
    "ALTER TABLE memory_receipts ADD KEY IF NOT EXISTS brief_receipt_order(scope_id,generation,sequence)",
    "ALTER TABLE memory_units ADD UNIQUE KEY IF NOT EXISTS brief_unit_generation(id,scope_id,generation)",
    "CREATE TABLE IF NOT EXISTS memory_brief_state ("
    " scope_id VARCHAR(40) NOT NULL,generation BIGINT UNSIGNED NOT NULL,"
    " t1_sequence BIGINT NOT NULL DEFAULT -1,t1_byte_end BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " owner VARCHAR(63) NOT NULL DEFAULT '',fence BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    " attempts INT NOT NULL DEFAULT 0,lease_until DATETIME(6) NULL,retry_after DATETIME(6) NULL,"
    " PRIMARY KEY(scope_id,generation),CHECK(attempts BETWEEN 0 AND 3 AND t1_sequence>=-1),"
    " FOREIGN KEY(scope_id) REFERENCES memory_scopes(id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS memory_briefs ("
    " id CHAR(64) PRIMARY KEY,scope_id VARCHAR(40) NOT NULL,project_id VARCHAR(40) NOT NULL,"
    " conversation_id VARCHAR(40) NOT NULL,history_epoch BIGINT UNSIGNED NOT NULL,"
    " policy_epoch BIGINT UNSIGNED NOT NULL,generation BIGINT UNSIGNED NOT NULL,"
    " begin_sequence BIGINT NOT NULL,begin_byte BIGINT UNSIGNED NOT NULL,"
    " end_sequence BIGINT NOT NULL,end_byte BIGINT UNSIGNED NOT NULL,"
    " previous_sequence BIGINT NOT NULL,previous_byte_end BIGINT UNSIGNED NOT NULL,"
    " input_digest CHAR(64) NOT NULL,tokenizer VARCHAR(191) NOT NULL,model_fingerprint CHAR(64) NOT NULL,"
    " prompt_schema VARCHAR(96) NOT NULL,summary_json VARCHAR(1536) NOT NULL,summary_tokens INT NOT NULL,"
    " input_count INT NOT NULL,created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),"
    " CHECK(OCTET_LENGTH(summary_json)<=1536 AND summary_tokens BETWEEN 0 AND 256 AND input_count BETWEEN 1 AND 4),"
    " UNIQUE KEY brief_scope_generation(id,scope_id,generation),"
    " KEY brief_recall(scope_id,generation,end_sequence,id),"
    " FOREIGN KEY(scope_id,project_id,conversation_id) REFERENCES memory_scopes(id,project_id,conversation_id) ON DELETE CASCADE"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    "CREATE TABLE IF NOT EXISTS memory_brief_inputs ("
    " brief_id CHAR(64) NOT NULL,ordinal INT NOT NULL,unit_id CHAR(64) NOT NULL,"
    " scope_id VARCHAR(40) NOT NULL,generation BIGINT UNSIGNED NOT NULL,"
    " source_hash CHAR(64) NOT NULL,text_hash CHAR(64) NOT NULL,"
    " PRIMARY KEY(brief_id,ordinal),UNIQUE KEY brief_original(brief_id,unit_id),CHECK(ordinal BETWEEN 1 AND 4),"
    " FOREIGN KEY(brief_id,scope_id,generation) REFERENCES memory_briefs(id,scope_id,generation) ON DELETE CASCADE,"
    " FOREIGN KEY(unit_id,scope_id,generation) REFERENCES memory_units(id,scope_id,generation) ON DELETE RESTRICT"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
    NULL,
};

#endif
