CREATE TABLE IF NOT EXISTS schema_version (
    version    INT PRIMARY KEY,
    applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS node (
    server_id      VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    server_name    VARCHAR(100) NOT NULL,
    currency       VARCHAR(40)  NOT NULL,
    plugin_ver     VARCHAR(24)  NOT NULL,
    last_heartbeat BIGINT       NOT NULL,
    status         VARCHAR(16)  NOT NULL
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS rate (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    server_id   VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    rate_bigint BIGINT      NOT NULL CHECK (rate_bigint > 0),
    effective_at BIGINT     NOT NULL,
    set_by      VARCHAR(100) NOT NULL,
    INDEX rate_current (server_id, effective_at, id),
    FOREIGN KEY (server_id) REFERENCES node (server_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS config (
    k          VARCHAR(80) PRIMARY KEY,
    v          JSON        NOT NULL,
    version    BIGINT      NOT NULL DEFAULT 1,
    updated_by VARCHAR(100) NOT NULL,
    updated_at TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS card (
    card_no      VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    xuid         VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    slot_index   INT         NOT NULL,
    balance_base BIGINT      NOT NULL DEFAULT 0 CHECK (balance_base >= 0),
    version      BIGINT      NOT NULL DEFAULT 0,
    status       VARCHAR(16) NOT NULL DEFAULT 'ACTIVE',
    live_slot    INT GENERATED ALWAYS AS (IF(status = 'CLOSED', NULL, slot_index)) STORED,
    UNIQUE KEY card_slot (xuid, live_slot),
    INDEX card_owner (xuid, status)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS card_lot (
    id               CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    card_no          VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    base_amount      BIGINT      NOT NULL CHECK (base_amount > 0),
    remain_base      BIGINT      NOT NULL,
    origin_server_id VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    created_at       BIGINT      NOT NULL,
    CHECK (remain_base >= 0 AND remain_base <= base_amount),
    INDEX lot_spend (card_no, created_at, id),
    FOREIGN KEY (card_no) REFERENCES card (card_no),
    FOREIGN KEY (origin_server_id) REFERENCES node (server_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS intent (
    intent_id     CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    xuid          VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    dir           VARCHAR(16) NOT NULL,
    card_no       VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    base_amount   BIGINT      NOT NULL CHECK (base_amount > 0),
    fee_base      BIGINT      NOT NULL CHECK (fee_base >= 0),
    local_amount  BIGINT      NOT NULL,
    rate          BIGINT      NOT NULL,
    server_id     VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    state         VARCHAR(24) NOT NULL,
    attempts      INT         NOT NULL DEFAULT 0,
    error         TEXT        NOT NULL,
    request_key   VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NULL UNIQUE,
    peer_xuid     VARCHAR(20) NOT NULL DEFAULT '',
    lot_snapshot  JSON        NOT NULL,
    created_at    BIGINT      NOT NULL,
    updated_at    BIGINT      NOT NULL,
    INDEX intent_recovery (state, updated_at),
    INDEX intent_owner (xuid, state),
    FOREIGN KEY (card_no) REFERENCES card (card_no),
    FOREIGN KEY (server_id) REFERENCES node (server_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS ledger (
    id            CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    xuid          VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    card_no       VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    kind          VARCHAR(32) NOT NULL,
    delta_base    BIGINT      NOT NULL,
    fee_base      BIGINT      NOT NULL,
    balance_after BIGINT      NOT NULL,
    rate          BIGINT      NOT NULL,
    local_amount  BIGINT      NOT NULL,
    server_id     VARCHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    peer_xuid     VARCHAR(20) NOT NULL,
    intent_id     CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    created_at    BIGINT      NOT NULL,
    INDEX ledger_owner (xuid, created_at, id),
    FOREIGN KEY (card_no) REFERENCES card (card_no),
    FOREIGN KEY (intent_id) REFERENCES intent (intent_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS audit_legacy (
    id            CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
    xuid          VARCHAR(20) NOT NULL,
    evt           VARCHAR(16) NOT NULL,
    delta         BIGINT      NULL,
    balance_after BIGINT      NOT NULL,
    server_id     VARCHAR(40) NOT NULL,
    created_at    BIGINT      NOT NULL,
    INDEX audit_owner (xuid, created_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS card_gen_seq (
    k VARCHAR(80) PRIMARY KEY,
    v BIGINT      NOT NULL
) ENGINE=InnoDB;

