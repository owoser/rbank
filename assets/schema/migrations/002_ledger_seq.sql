-- 流水同一秒内可能有多条，created_at 无法定序；用自增列提供单调顺序。
ALTER TABLE ledger ADD COLUMN seq BIGINT AUTO_INCREMENT UNIQUE FIRST;
