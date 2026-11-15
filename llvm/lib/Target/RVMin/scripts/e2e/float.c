// Soft float: every operation is a libcall (chapter 10).
float fadd(float a, float b) { return a + b; }
float fsub(float a, float b) { return a - b; }
float fmul(float a, float b) { return a * b; }
float fdiv(float a, float b) { return a / b; }

double dadd(double a, double b) { return a + b; }
double dmul(double a, double b) { return a * b; }

int fcmp(float a, float b) {
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

int conversions(int i, float f, double d) {
  int a = (int)f;
  float b = (float)i;
  double c = (double)f;
  float e = (float)d;
  return a + (int)b + (int)c + (int)e;
}

double mixed(float a, double b) { return a * b + a / b; }
