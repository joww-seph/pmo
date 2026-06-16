/*
 * sketch_cam.ino — ESP32-CAM occupancy detection unit
 *
 * PURPOSE
 * -------
 * This sketch runs on a separate AI Thinker ESP32-CAM module. It continuously
 * captures frames from the OV2640 camera, runs them through an Edge Impulse
 * person-detection CNN, and sends the result to the main ESP32 (sketch_pmo)
 * over UART2 at 9600 baud.
 *
 * OUTPUT PROTOCOL
 * ---------------
 * After every inference the camera sends one of two plain-text lines:
 *   "PERSON_DETECTED\n"  — a person is present with confidence >= 0.6
 *   "NO_PERSON\n"        — no person detected
 *
 * The main ESP32 listens on GPIO 36 (CAM_RX_PIN) and trips the relay to save
 * energy if no person is detected for 60 consecutive seconds.
 *
 * INFERENCE PIPELINE (per loop iteration)
 * ----------------------------------------
 *  1. Allocate a 320x240x3 byte frame buffer in PSRAM.
 *  2. Capture a JPEG frame from the OV2640 sensor.
 *  3. Decode JPEG to raw RGB888 in-place (fmt2rgb888).
 *  4. If the model input dimensions differ from 320x240, crop and bicubic-
 *     interpolate the frame to the required size (crop_and_interpolate_rgb888).
 *  5. Package the buffer as an Edge Impulse signal_t and call run_classifier().
 *  6. Parse the result:
 *     - Object-detection mode: iterate bounding boxes; any box labelled
 *       "person" with value > 0 counts as detected.
 *     - Classification mode:   check the "person" category confidence
 *       against a 0.6 threshold.
 *  7. Print result to COMM_SERIAL (GPIO 13 TX → main ESP32 GPIO 36 RX).
 *  8. Free the frame buffer and repeat.
 *
 * HARDWARE CONNECTIONS (AI Thinker ESP32-CAM)
 * --------------------------------------------
 *   COMM_SERIAL TX (GPIO 13) → Main ESP32 GPIO 36 (CAM_RX_PIN)
 *   GND → GND (common ground between both boards required)
 *   3.3 V supply — power from its own regulator or shared 3.3 V rail
 */

#include <Human_Detection_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"

// Select the camera module hardware pinout.
// Only CAMERA_MODEL_AI_THINKER is wired for this project.
#define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_ESP_EYE)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23
#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      14
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM   5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

#elif defined(CAMERA_MODEL_AI_THINKER)
// AI Thinker ESP32-CAM — OV2640 sensor GPIO map
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#else
#error "Camera model not selected"
#endif

// Raw capture resolution before resizing to the model input dimensions.
// QVGA (320x240) gives sufficient detail for person detection while keeping
// JPEG decode time short enough for a responsive inference loop.
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS           320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS           240
#define EI_CAMERA_FRAME_BYTE_SIZE                 3    // RGB888: 3 bytes per pixel

// UART2 channel used to send results to the main ESP32.
// TX = GPIO 13, which is shared with the SD card slot on the AI Thinker board.
// Remove the SD card before flashing to avoid conflicts.
#define COMM_SERIAL Serial2
#define COMM_BAUD   9600

static bool debug_nn = false;      // Set true to print per-class scores to Serial
static bool is_initialised = false;
uint8_t *snapshot_buf;             // Heap-allocated per loop iteration in PSRAM

// Camera hardware configuration — QVGA JPEG, stored in PSRAM frame buffer.
static camera_config_t camera_config = {
    .pin_pwdn  = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk  = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href  = HREF_GPIO_NUM,
    .pin_pclk  = PCLK_GPIO_NUM,
    .xclk_freq_hz = 20000000,         // 20 MHz XCLK — required by OV2640
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,   // Capture as JPEG; decode to RGB888 in software
    .frame_size   = FRAMESIZE_QVGA,   // 320x240
    .jpeg_quality = 12,               // 0 = best, 63 = worst; 12 balances speed vs quality
    .fb_count     = 1,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

bool ei_camera_init(void);
void ei_camera_deinit(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf);

void setup()
{
    Serial.begin(115200);
    // COMM_SERIAL TX is GPIO 13; RX pin (-1) is unused — we only send, never receive.
    COMM_SERIAL.begin(COMM_BAUD, SERIAL_8N1, -1, 13);
    while (!Serial);
    Serial.println("Edge Impulse Inferencing Demo");
    if (ei_camera_init() == false) {
        ei_printf("Failed to initialize Camera!\r\n");
    }
    else {
        ei_printf("Camera initialized\r\n");
    }
    ei_printf("\nStarting continious inference in 2 seconds...\n");
    ei_sleep(2000);
}

void loop()
{
    // 5 ms yield — keeps the FreeRTOS watchdog happy between inference cycles.
    if (ei_sleep(5) != EI_IMPULSE_OK) {
        return;
    }

    // Allocate the raw frame buffer in PSRAM for this inference cycle.
    // It is freed at the end of the loop to avoid a permanent reservation.
    snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);

    if(snapshot_buf == nullptr) {
        ei_printf("ERR: Failed to allocate snapshot buffer!\n");
        return;
    }

    // Build the Edge Impulse signal descriptor that points to our frame buffer.
    // total_length = model input width × height (in pixels, not bytes).
    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;  // callback that unpacks RGB888 → float

    // Capture and resize the frame to the model's expected input dimensions.
    if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
        ei_printf("Failed to capture image\r\n");
        free(snapshot_buf);
        return;
    }

    ei_impulse_result_t result = { 0 };

    // Run the Edge Impulse CNN inference (DSP + classification pipeline).
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", err);
        return;
    }

    ei_printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.): \n",
                result.timing.dsp, result.timing.classification, result.timing.anomaly);

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    // Object-detection model: iterate bounding boxes and look for the "person" label.
    // Any bounding box labelled "person" with a non-zero confidence triggers detection.
    ei_printf("Object detection bounding boxes:\r\n");
    bool person_found = false;
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
        if (strcmp(bb.label, "person") == 0) {
            person_found = true;
        }
    }
    // Broadcast result to main ESP32 over UART.
    COMM_SERIAL.println(person_found ? "PERSON_DETECTED" : "NO_PERSON");
    Serial.println(person_found ? "PERSON_DETECTED" : "NO_PERSON");

#else
    // Classification model: check the "person" category's confidence score.
    // Threshold 0.6 was chosen to balance false-negative (missing a person)
    // against false-positive (triggering when the room is empty).
    ei_printf("Predictions:\r\n");
    bool person_found = false;
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ei_printf("  %s: ", ei_classifier_inferencing_categories[i]);
        ei_printf("%.5f\r\n", result.classification[i].value);
        if (strcmp(ei_classifier_inferencing_categories[i], "person") == 0
            && result.classification[i].value >= 0.6f) {
            person_found = true;
        }
    }
    COMM_SERIAL.println(person_found ? "PERSON_DETECTED" : "NO_PERSON");
    Serial.println(person_found ? "PERSON_DETECTED" : "NO_PERSON");
#endif

#if EI_CLASSIFIER_HAS_ANOMALY
    ei_printf("Anomaly prediction: %.3f\r\n", result.anomaly);
#endif

#if EI_CLASSIFIER_HAS_VISUAL_ANOMALY
    ei_printf("Visual anomalies:\r\n");
    for (uint32_t i = 0; i < result.visual_ad_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.visual_ad_grid_cells[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    }
#endif

    free(snapshot_buf);
}

/*
 * ei_camera_init — configure and start the OV2640 sensor.
 * Applies flip and brightness corrections for models that were trained on
 * upright images — without the vflip on OV3660 sensors the model would see
 * an upside-down frame and confidence scores would be unreliable.
 */
bool ei_camera_init(void) {

    if (is_initialised) return true;

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }

    sensor_t * s = esp_camera_sensor_get();
    // OV3660 sensors are physically mounted inverted; flip + brightness adjustments
    // align the captured image orientation with the training dataset.
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);
      s->set_brightness(s, 1);
      s->set_saturation(s, 0);
    }

#if defined(CAMERA_MODEL_M5STACK_WIDE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#elif defined(CAMERA_MODEL_ESP_EYE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    s->set_awb_gain(s, 1);
#endif

    is_initialised = true;
    return true;
}

void ei_camera_deinit(void) {

    esp_err_t err = esp_camera_deinit();

    if (err != ESP_OK)
    {
        ei_printf("Camera deinit failed\n");
        return;
    }

    is_initialised = false;
    return;
}

/*
 * ei_camera_capture — grab one JPEG frame, decode it to RGB888, and optionally
 * resize it to match the model's input dimensions.
 *
 * fmt2rgb888 decodes the JPEG in-place into snapshot_buf (already allocated).
 * If the raw capture resolution does not match EI_CLASSIFIER_INPUT_WIDTH/HEIGHT,
 * crop_and_interpolate_rgb888 performs a centre-crop + bilinear resize so that
 * the aspect ratio is preserved and the model always receives its expected shape.
 */
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    bool do_resize = false;

    if (!is_initialised) {
        ei_printf("ERR: Camera is not initialized\r\n");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        ei_printf("Camera capture failed\n");
        return false;
    }

   bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);

   esp_camera_fb_return(fb);

   if(!converted){
       ei_printf("Conversion failed\n");
       return false;
   }

    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS)
        || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        do_resize = true;
    }

    if (do_resize) {
        ei::image::processing::crop_and_interpolate_rgb888(
        out_buf,
        EI_CAMERA_RAW_FRAME_BUFFER_COLS,
        EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
        out_buf,
        img_width,
        img_height);
    }

    return true;
}

/*
 * ei_camera_get_data — Edge Impulse data callback.
 *
 * The SDK calls this function to read pixel values from the frame buffer into
 * the float array used by the classifier. Each RGB888 pixel is packed into a
 * single float as 0x00RRGGBB (big-endian channel order expected by the model).
 * offset and length are in pixels, not bytes.
 */
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        // Pack R, G, B channels into a single float value (0x00RRGGBB layout).
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];
        out_ptr_ix++;
        pixel_ix+=3;
        pixels_left--;
    }
    return 0;
}

// Compile-time guard: this sketch only works with camera-type Edge Impulse models.
#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif
