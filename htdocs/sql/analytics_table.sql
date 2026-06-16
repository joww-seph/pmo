-- Run this once to create the AI recommendations table
CREATE TABLE IF NOT EXISTS ai_recommendations (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    generated_at    DATETIME NOT NULL,
    period          VARCHAR(10) NOT NULL COMMENT 'morning or evening',
    energy_rating   VARCHAR(5) NOT NULL DEFAULT '?',
    rating_score    INT NOT NULL DEFAULT 0,
    predicted_bill  DECIMAL(10,2) NOT NULL DEFAULT 0,
    recommendations TEXT NOT NULL,
    efficiency_tip  TEXT,
    summary         TEXT,
    raw_response    TEXT,
    avg_voltage     DECIMAL(8,4),
    avg_power       DECIMAL(8,4),
    today_energy    DECIMAL(10,6),
    avg_pf          DECIMAL(8,6),
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_generated_at (generated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
