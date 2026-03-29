-- phpMyAdmin SQL Dump
-- version 4.9.0.1
-- https://www.phpmyadmin.net/
--
-- Host: sql302.infinityfree.com
-- Generation Time: Mar 29, 2026 at 06:46 AM
-- Server version: 11.4.10-MariaDB
-- PHP Version: 7.2.22

SET SQL_MODE = "NO_AUTO_VALUE_ON_ZERO";
SET AUTOCOMMIT = 0;
START TRANSACTION;
SET time_zone = "+00:00";


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;

--
-- Database: `if0_41381183_pmo_db`
--

-- --------------------------------------------------------

--
-- Table structure for table `relay_commands`
--

CREATE TABLE `relay_commands` (
  `id` int(11) NOT NULL,
  `device_id` varchar(50) NOT NULL DEFAULT 'PMO-ESP32-001',
  `command` tinyint(1) NOT NULL,
  `issued_at` timestamp NULL DEFAULT current_timestamp(),
  `executed` tinyint(1) DEFAULT 0
) ENGINE=MyISAM DEFAULT CHARSET=latin1 COLLATE=latin1_swedish_ci;

--
-- Dumping data for table `relay_commands`
--

INSERT INTO `relay_commands` (`id`, `device_id`, `command`, `issued_at`, `executed`) VALUES
(1, 'PMO-ESP32-001', 0, '2026-03-29 09:38:30', 1),
(2, 'PMO-ESP32-001', 1, '2026-03-29 09:38:44', 1);

-- --------------------------------------------------------

--
-- Table structure for table `sensor_readings`
--

CREATE TABLE `sensor_readings` (
  `id` int(11) NOT NULL,
  `device_id` varchar(50) NOT NULL DEFAULT 'PMO-ESP32-001',
  `wifi_ssid` varchar(100) DEFAULT NULL,
  `voltage` float DEFAULT NULL,
  `current_a` float DEFAULT NULL,
  `power_w` float DEFAULT NULL,
  `energy_kwh` float DEFAULT NULL,
  `frequency_hz` float DEFAULT NULL,
  `power_factor` float DEFAULT NULL,
  `apparent_power` float DEFAULT NULL,
  `reactive_power` float DEFAULT NULL,
  `load_type` varchar(20) DEFAULT NULL,
  `cost_per_hour` float DEFAULT NULL,
  `cost_per_month` float DEFAULT NULL,
  `wasted_power` float DEFAULT NULL,
  `safety_margin` float DEFAULT NULL,
  `co2_kg` float DEFAULT NULL,
  `relay_state` tinyint(1) DEFAULT 1,
  `trip_reason` varchar(30) DEFAULT 'None',
  `recorded_at` datetime DEFAULT NULL
) ENGINE=MyISAM DEFAULT CHARSET=latin1 COLLATE=latin1_swedish_ci;

--
-- Dumping data for table `sensor_readings`
--

INSERT INTO `sensor_readings` (`id`, `device_id`, `wifi_ssid`, `voltage`, `current_a`, `power_w`, `energy_kwh`, `frequency_hz`, `power_factor`, `apparent_power`, `reactive_power`, `load_type`, `cost_per_hour`, `cost_per_month`, `wasted_power`, `safety_margin`, `co2_kg`, `relay_state`, `trip_reason`, `recorded_at`) VALUES
(1, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:30'),
(2, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.6, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:34'),
(3, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.8, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:38'),
(4, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.8, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:42'),
(5, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.1, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:46'),
(6, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.3, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:51'),
(7, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.3, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:55'),
(8, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.3, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:37:59'),
(9, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.3, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:03'),
(10, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.4, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:07'),
(11, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.4, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:11'),
(12, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.5, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:15'),
(13, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.4, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:19'),
(14, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.5, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:23'),
(15, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.2, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:26'),
(16, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.1, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:30'),
(17, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.2, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:34'),
(18, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.9, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 0, 'None', '2026-03-29 17:38:38'),
(19, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.9, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 0, 'None', '2026-03-29 17:38:42'),
(20, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.7, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 0, 'None', '2026-03-29 17:38:46'),
(21, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.9, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:50'),
(22, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.8, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:53'),
(23, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:38:57'),
(24, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:39:01'),
(25, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.2, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:39:05'),
(26, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:39:08'),
(27, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225.2, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:39:12'),
(28, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 225, 0.072, 8.6, 0.016, 59.8, 0.53, 16.2, 13.7288, 'Inductive', 0.118818, 85.5493, 7.6, 99.55, 0.009651, 1, 'None', '2026-03-29 17:39:16'),
(29, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.8, 0.071, 8.5, 0.016, 59.8, 0.53, 15.9608, 13.5091, 'Inductive', 0.117437, 84.5545, 7.4608, 99.5563, 0.009651, 1, 'None', '2026-03-29 17:39:20'),
(30, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.7, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:39:24'),
(31, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.6, 0.071, 8.5, 0.016, 59.8, 0.53, 15.9466, 13.4924, 'Inductive', 0.117437, 84.5545, 7.4466, 99.5563, 0.009651, 1, 'None', '2026-03-29 17:39:28'),
(32, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.8, 0.071, 8.5, 0.016, 59.8, 0.53, 15.9608, 13.5091, 'Inductive', 0.117437, 84.5545, 7.4608, 99.5563, 0.009651, 1, 'None', '2026-03-29 17:39:32'),
(33, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.4, 0.07, 8.4, 0.016, 59.8, 0.53, 15.708, 13.2733, 'Inductive', 0.116055, 83.5598, 7.308, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:39:35'),
(34, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.07, 8.4, 0.016, 59.8, 0.53, 15.715, 13.2816, 'Inductive', 0.116055, 83.5598, 7.315, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:39:39'),
(35, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.6, 0.07, 8.4, 0.016, 59.8, 0.53, 15.722, 13.2899, 'Inductive', 0.116055, 83.5598, 7.322, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:39:43'),
(36, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.069, 8.4, 0.016, 59.8, 0.54, 15.4905, 13.0152, 'Inductive', 0.116055, 83.5598, 7.0905, 99.5687, 0.009651, 1, 'None', '2026-03-29 17:39:47'),
(37, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.4, 0.07, 8.4, 0.016, 59.8, 0.53, 15.708, 13.2733, 'Inductive', 0.116055, 83.5598, 7.308, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:39:50'),
(38, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224, 0.07, 8.4, 0.016, 59.8, 0.54, 15.68, 13.2402, 'Inductive', 0.116055, 83.5598, 7.28, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:39:55'),
(39, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.069, 8.4, 0.016, 59.8, 0.54, 15.4905, 13.0152, 'Inductive', 0.116055, 83.5598, 7.0905, 99.5687, 0.009651, 1, 'None', '2026-03-29 17:40:00'),
(40, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.4, 0.07, 8.4, 0.016, 59.7, 0.53, 15.708, 13.2733, 'Inductive', 0.116055, 83.5598, 7.308, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:40:04'),
(41, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.3, 0.069, 8.3, 0.016, 59.8, 0.54, 15.4767, 13.0629, 'Inductive', 0.114674, 82.565, 7.1767, 99.5687, 0.009651, 1, 'None', '2026-03-29 17:40:08'),
(42, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.2, 0.069, 8.3, 0.016, 59.8, 0.54, 15.4698, 13.0547, 'Inductive', 0.114674, 82.565, 7.1698, 99.5687, 0.009651, 1, 'None', '2026-03-29 17:40:12'),
(43, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.069, 8.3, 0.016, 59.8, 0.54, 15.4905, 13.0792, 'Inductive', 0.114674, 82.565, 7.1905, 99.5687, 0.009651, 1, 'None', '2026-03-29 17:40:15'),
(44, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.07, 8.4, 0.016, 59.8, 0.53, 15.715, 13.2816, 'Inductive', 0.116055, 83.5598, 7.315, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:40:19'),
(45, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 50.1, 0.022, 2, 0.016, 59.8, 1, 1.1022, 0, 'Resistive', 0.027632, 19.8952, -0.8978, 99.8625, 0.009651, 0, 'UnderVolt', '2026-03-29 17:40:23'),
(46, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 50.1, 0.022, 2, 0.016, 59.8, 1, 1.1022, 0, 'Resistive', 0.027632, 19.8952, -0.8978, 99.8625, 0.009651, 0, 'UnderVolt', '2026-03-29 17:40:27'),
(47, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 50.1, 0.022, 2, 0.016, 59.8, 1, 1.1022, 0, 'Resistive', 0.027632, 19.8952, -0.8978, 99.8625, 0.009651, 0, 'UnderVolt', '2026-03-29 17:40:31'),
(48, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 50.1, 0.022, 2, 0.016, 59.8, 1, 1.1022, 0, 'Resistive', 0.027632, 19.8952, -0.8978, 99.8625, 0.009651, 0, 'UnderVolt', '2026-03-29 17:40:34'),
(49, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 50.1, 0.022, 2, 0.016, 59.8, 1, 1.1022, 0, 'Resistive', 0.027632, 19.8952, -0.8978, 99.8625, 0.009651, 0, 'UnderVolt', '2026-03-29 17:40:38'),
(50, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 50.1, 0.022, 2, 0.016, 59.8, 1, 1.1022, 0, 'Resistive', 0.027632, 19.8952, -0.8978, 99.8625, 0.009651, 0, 'UnderVolt', '2026-03-29 17:40:42'),
(51, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 1624.3, 2155430, 256316000, 528246, 2200.9, 304.49, 3501070000, 3491670000, 'Resistive', 3541280, 2549720000, 3244750000, -13471400, 318638, 1, 'None', '2026-03-29 17:40:46'),
(52, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.4, 0, 0, 0.016, 59.8, 0, 0, 0, 'Reactive', 0, 0, 0, 100, 0.009651, 1, 'None', '2026-03-29 17:40:50'),
(53, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.2, 0.071, 8.4, 0.016, 59.8, 0.53, 15.9182, 13.5214, 'Inductive', 0.116055, 83.5598, 7.5182, 99.5563, 0.009651, 1, 'None', '2026-03-29 17:40:55'),
(54, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.3, 0.07, 8.4, 0.016, 59.8, 0.53, 15.701, 13.265, 'Inductive', 0.116055, 83.5598, 7.301, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:00'),
(55, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.3, 0.07, 8.4, 0.016, 59.8, 0.53, 15.701, 13.265, 'Inductive', 0.116055, 83.5598, 7.301, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:04'),
(56, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.1, 0.07, 8.4, 0.016, 59.8, 0.54, 15.687, 13.2485, 'Inductive', 0.116055, 83.5598, 7.287, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:09'),
(57, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.3, 0.071, 8.3, 0.016, 59.8, 0.52, 15.9253, 13.5914, 'Inductive', 0.114674, 82.565, 7.6253, 99.5563, 0.009651, 1, 'None', '2026-03-29 17:41:14'),
(58, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.6, 0.07, 8.3, 0.016, 59.8, 0.53, 15.722, 13.3526, 'Inductive', 0.114674, 82.565, 7.422, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:22'),
(59, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.7, 0.07, 8.2, 0.016, 59.8, 0.52, 15.729, 13.4224, 'Inductive', 0.113292, 81.5703, 7.529, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:26'),
(60, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.6, 0.07, 8.3, 0.016, 59.8, 0.53, 15.722, 13.3526, 'Inductive', 0.114674, 82.565, 7.422, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:30'),
(61, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.07, 8.3, 0.016, 59.8, 0.53, 15.715, 13.3443, 'Inductive', 0.114674, 82.565, 7.415, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:34'),
(62, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.5, 0.07, 8.2, 0.016, 59.8, 0.52, 15.715, 13.406, 'Inductive', 0.113292, 81.5703, 7.515, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:39'),
(63, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.3, 0.07, 8.2, 0.016, 59.8, 0.52, 15.701, 13.3896, 'Inductive', 0.113292, 81.5703, 7.501, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:43'),
(64, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 224.2, 0.07, 8.2, 0.016, 59.8, 0.52, 15.694, 13.3814, 'Inductive', 0.113292, 81.5703, 7.494, 99.5625, 0.009651, 1, 'None', '2026-03-29 17:41:48'),
(65, 'PMO-ESP32-001', 'HUAWEI-2.4G-S8Kc', 25.7, 0, 1.1, 0.016, 59.8, 1, 0, 0, 'Resistive', 0.015198, 10.9424, -1.1, 100, 0.009651, 0, 'UnderVolt', '2026-03-29 17:41:52');

--
-- Indexes for dumped tables
--

--
-- Indexes for table `relay_commands`
--
ALTER TABLE `relay_commands`
  ADD PRIMARY KEY (`id`);

--
-- Indexes for table `sensor_readings`
--
ALTER TABLE `sensor_readings`
  ADD PRIMARY KEY (`id`);

--
-- AUTO_INCREMENT for dumped tables
--

--
-- AUTO_INCREMENT for table `relay_commands`
--
ALTER TABLE `relay_commands`
  MODIFY `id` int(11) NOT NULL AUTO_INCREMENT, AUTO_INCREMENT=3;

--
-- AUTO_INCREMENT for table `sensor_readings`
--
ALTER TABLE `sensor_readings`
  MODIFY `id` int(11) NOT NULL AUTO_INCREMENT, AUTO_INCREMENT=66;
COMMIT;

/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
