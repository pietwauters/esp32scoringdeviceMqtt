#include <cmath>

class ModelSolver {
   public:
    // Constructor
    ModelSolver(double Vmax_, double R1_, double R2_) : Vmax(Vmax_), R1(R1_), R2(R2_) {}

    // Compute V(x)
    double compute_V_SingleEnded(double x) const {
        
        return Vmax * R1 / (R1 + R2+ x);
    }

    // Compute x(V)
    double compute_x(double V) const {
        if(V <= 0 || V >= Vmax) {
            return 0; // Invalid voltage, return 0
        }
        return (Vmax / V - 1.0) * R1 - R2;
    }

    // Setters
    void set_R1(double R1_) { R1 = R1_; }
    void set_R2(double R2_) { R2 = R2_; }
    void set_Vmax(double Vmax_) { Vmax = Vmax_; }

    // Getters
    double get_R1() const { return R1; }
    double get_R2() const { return R2; }
    double get_Vmax() const { return Vmax; }

   private:
    double Vmax;
    double R1;
    double R2;

};
