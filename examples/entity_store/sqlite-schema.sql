-- 部署时执行的 SQLite 迁移；日常业务通过 entity_store 服务读取和保存。
CREATE TABLE IF NOT EXISTS orders (
    order_id TEXT PRIMARY KEY NOT NULL,
    status TEXT NOT NULL,
    paid_cents INTEGER NOT NULL DEFAULT 0,
    memo TEXT NULL,
    version INTEGER NOT NULL DEFAULT 1 CHECK (version >= 1)
);

CREATE TABLE IF NOT EXISTS payments (
    payment_id TEXT PRIMARY KEY NOT NULL,
    state TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 1 CHECK (version >= 1)
);
