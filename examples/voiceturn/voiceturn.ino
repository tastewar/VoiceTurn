/* VoiceTurn - Voice-controlled turn lights for a safer ride
   Copyright (c) 2021 Alvaro Gonzalez-Vila

   Based on:
   Edge Impulse Arduino examples
   Copyright (c) 2021 EdgeImpulse Inc.

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

// If your target is limited in memory remove this macro to save 10K RAM
#define EIDSP_QUANTIZE_FILTERBANK   0

/**
   Define the number of slices per model window. E.g. a model window of 1000 ms
   with slices per model window set to 4. Results in a slice size of 250 ms.
   For more info: https://docs.edgeimpulse.com/docs/continuous-audio-sampling
*/
#define EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW 1

/* LED strip pinout */
#define LED_PIN_LEFT 4
#define LED_PIN_RIGHT 7
#define LED_COUNT 6

/* Built-in RGB LED pinout */
#define RGB_GREEN 22
#define RGB_RED 23
#define RGB_BLUE 24

/* Includes ---------------------------------------------------------------- */
#include <PDM.h>
#include <tastewar-project-1_inferencing.h>
#include <Adafruit_NeoPixel.h>

/* LED strips for left and right signals */
Adafruit_NeoPixel left(LED_COUNT, LED_PIN_LEFT, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel right(LED_COUNT, LED_PIN_RIGHT, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel onePixel = Adafruit_NeoPixel(1, 8, NEO_GRB + NEO_KHZ800);

static int period = 100; // Time between two individual LED lightings
static int cycles = 3; // Number of lighting cycles per voice command

/** Audio buffers, pointers and selectors */
typedef struct {
  signed short *buffers[2];
  unsigned char buf_select;
  unsigned char buf_ready;
  unsigned int buf_count;
  unsigned int n_samples;
} inference_t;

enum
{
  rightturn,
  leftturn,
} direction;

static inference_t inference;
static bool record_ready = false;
static signed short *sampleBuffer;
static bool debug_nn = false; // Set this to true to see e.g. features generated from the raw signal
static int print_results = -(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW);

namespace std {void __throw_out_of_range_fmt(const char*, ...) { while(1); }; }

/**
   @brief      Arduino setup function
*/
void setup()
{
  // Initialization of LED strips:
  left.begin();
  right.begin();
  left.show();
  right.show();

  // Set BRIGHTNESS to about 2/5 (max = 255)
  left.setBrightness(100);
  right.setBrightness(100);

  // Initialization of built-in RGB LED
  pinMode(RGB_RED, OUTPUT);
  pinMode(RGB_GREEN, OUTPUT);
  pinMode(RGB_BLUE, OUTPUT);

  onePixel.begin();             // Start the NeoPixel object
  onePixel.clear();             // Set NeoPixel color to black (0,0,0)
  onePixel.setBrightness(20);   // Affects all subsequent settings
  onePixel.show();              // Update the pixel state

  Serial.begin(115200);

  Serial.println("VoiceTurn Inferencing");

  // summary of inferencing settings (from model_metadata.h)
  ei_printf("Inferencing settings:\n");
  ei_printf("\tInterval: %.2f ms.\n", (float)EI_CLASSIFIER_INTERVAL_MS);
  ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
  ei_printf("\tSample length: %d ms.\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
  ei_printf("\tNo. of classes: %d\n", sizeof(ei_classifier_inferencing_categories) /
            sizeof(ei_classifier_inferencing_categories[0]));

  run_classifier_init();
  if (microphone_inference_start(EI_CLASSIFIER_SLICE_SIZE) == false) {
    ei_printf("ERR: Failed to setup audio sampling\r\n");
    return;
  }
}

/**
   @brief      Arduino main function. Runs the inferencing loop.
*/
void loop()
{
  rgb_green(); // Shows the board is READY

  bool m = microphone_inference_record();
  if (!m) {
    ei_printf("ERR: Failed to record audio...\n");
    return;
  }

  signal_t signal;
  signal.total_length = EI_CLASSIFIER_SLICE_SIZE;
  signal.get_data = &microphone_audio_signal_get_data;
  ei_impulse_result_t result = {0};

  EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, debug_nn);
  if (r != EI_IMPULSE_OK) {
    ei_printf("ERR: Failed to run classifier (%d)\n", r);
    return;
  }

  if (++print_results >= (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)) {
    // print the predictions
    ei_printf("Predictions ");
    ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
              result.timing.dsp, result.timing.classification, result.timing.anomaly);
    ei_printf(": \n");
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
      ei_printf("    %s: %.5f\n", result.classification[ix].label,
                result.classification[ix].value);
    }
#if EI_CLASSIFIER_HAS_ANOMALY == 1
    ei_printf("    anomaly score: %.3f\n", result.anomaly);
#endif

    print_results = 0;
  }

  // If the words LEFT[0] or RIGHT[1] are trustly detected, activate the corresponding LED strip
  if (result.classification[0].value >= 0.80) { // Trust threshold = 0.80 -> Word detected with a probability above 80%
    turn(leftturn);
  }
  if (result.classification[1].value >= 0.85) {
    turn(rightturn);
  }
}

/**
   @brief      Printf function uses vsnprintf and output using Arduino Serial

   @param[in]  format     Variable argument list
*/
void ei_printf(const char *format, ...) {
  static char print_buf[1024] = { 0 };

  va_list args;
  va_start(args, format);
  int r = vsnprintf(print_buf, sizeof(print_buf), format, args);
  va_end(args);

  if (r > 0) {
    Serial.write(print_buf);
  }
}

/**
   @brief      PDM buffer full callback
               Get data and call audio thread callback
*/
static void pdm_data_ready_inference_callback(void)
{
  int bytesAvailable = PDM.available();

  // read into the sample buffer
  int bytesRead = PDM.read((char *)&sampleBuffer[0], bytesAvailable);

  if (record_ready == true) {
    for (int i = 0; i<bytesRead >> 1; i++) {
      inference.buffers[inference.buf_select][inference.buf_count++] = sampleBuffer[i];

      if (inference.buf_count >= inference.n_samples) {
        inference.buf_select ^= 1;
        inference.buf_count = 0;
        inference.buf_ready = 1;
      }
    }
  }
}

/**
   @brief      Init inferencing struct and setup/start PDM

   @param[in]  n_samples  The n samples

   @return     { description_of_the_return_value }
*/
static bool microphone_inference_start(uint32_t n_samples)
{
  inference.buffers[0] = (signed short *)malloc(n_samples * sizeof(signed short));

  if (inference.buffers[0] == NULL) {
    return false;
  }

  inference.buffers[1] = (signed short *)malloc(n_samples * sizeof(signed short));

  if (inference.buffers[0] == NULL) {
    free(inference.buffers[0]);
    return false;
  }

  sampleBuffer = (signed short *)malloc((n_samples >> 1) * sizeof(signed short));

  if (sampleBuffer == NULL) {
    free(inference.buffers[0]);
    free(inference.buffers[1]);
    return false;
  }

  inference.buf_select = 0;
  inference.buf_count = 0;
  inference.n_samples = n_samples;
  inference.buf_ready = 0;

  // configure the data receive callback
  PDM.onReceive(&pdm_data_ready_inference_callback);

  // optionally set the gain, defaults to 20
  PDM.setGain(80);

  PDM.setBufferSize((n_samples >> 1) * sizeof(int16_t));

  // initialize PDM with:
  // - one channel (mono mode)
  // - a 16 kHz sample rate
  if (!PDM.begin(1, EI_CLASSIFIER_FREQUENCY)) {
    ei_printf("Failed to start PDM!");
  }

  record_ready = true;

  return true;
}

/**
   @brief      Wait on new data

   @return     True when finished
*/
static bool microphone_inference_record(void)
{
  bool ret = true;

  if (inference.buf_ready == 1) {
    ei_printf(
      "Error sample buffer overrun. Decrease the number of slices per model window "
      "(EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW)\n");
    ret = false;
  }

  while (inference.buf_ready == 0) {
    delay(1);
  }

  inference.buf_ready = 0;

  return ret;
}

/**
   Get raw audio signal data
*/
static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr)
{
  numpy::int16_to_float(&inference.buffers[inference.buf_select ^ 1][offset], out_ptr, length);

  return 0;
}

/**
   @brief      Stop PDM and release buffers
*/
static void microphone_inference_end(void)
{
  PDM.end();
  free(inference.buffers[0]);
  free(inference.buffers[1]);
  free(sampleBuffer);
}

/**
   @brief      Activate the turn lights

   @param[in]  strip  The LED strip to light on
*/
static void turn(int direction) {
  Adafruit_NeoPixel& strip = direction==rightturn?right:left;
  if ( direction == rightturn )
  {
    onePixel.setPixelColor(0, 100, 0, 0); // right == red
  }
  else
  {
    onePixel.setPixelColor(0, 0, 0, 100); // left == blue
  }
  onePixel.show();
  rgb_red(); // Shows the board is BUSY
  for (int i = 0; i < cycles; i++) {
    for (int j = 0; j < strip.numPixels(); j++) { // Lighting animation imitating the turn light signals of modern cars
      strip.setPixelColor(j, strip.Color(255, 104, 0)); // Color: Orange
      strip.show();
      delay(period);
    }
    strip.clear();
    strip.show();
    delay(2 * period); // Time between cycles is set to twice the time between two individual LED lightings
  }
  rgb_off(); // The board has FINISHED lighting the LED strip
  onePixel.clear();
  onePixel.show();
}

/**
   @brief      Built-in RGB LED: Red
*/
void rgb_red() {
  digitalWrite(RGB_RED, HIGH);
  digitalWrite(RGB_GREEN, LOW);
  digitalWrite(RGB_BLUE, LOW);
}

/**
   @brief      Built-in RGB LED: Green
*/
void rgb_green() {
  digitalWrite(RGB_RED, LOW);
  digitalWrite(RGB_GREEN, HIGH);
  digitalWrite(RGB_BLUE, LOW);
}

/**
   @brief      Built-in RGB LED: Off
*/
void rgb_off() {
  digitalWrite(RGB_RED, LOW);
  digitalWrite(RGB_GREEN, LOW);
  digitalWrite(RGB_BLUE, LOW);
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_MICROPHONE
#error "Invalid model for current sensor."
#endif
