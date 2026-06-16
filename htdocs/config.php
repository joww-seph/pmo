<?php
$env  = parse_ini_file(__DIR__ . '/.env');
$conn = new mysqli($env['DB_HOST'], $env['DB_USER'], $env['DB_PASS'], $env['DB_NAME']);
