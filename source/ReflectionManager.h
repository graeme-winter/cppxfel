#ifndef reflectionmanager
#define reflectionmanager

#include <vector>
#include <string>
#include <iostream>
#include "MtzManager.h"
#include "ScalingManager.h"

using namespace std;

struct ImageReflection
{
	Holder *holder;
	MtzManager *manager;
	double l;
};

class ReflectionManager
{
private:
	void holder_for_image(double l, Holder **holder);
	
	void alpha_beta_hj(vector<Scale_factor> Gs, int j, double *alpha_hj, double *beta_hj);
    CCP4SPG *group;
    vector<double> intensities;
    vector<double *> scales;
public:
	static void GForImage(ImageReflection *image, vector<Scale_factor> *Gs, Scale_factor **G, int *num = NULL);
    int refl_id;
	int prev_num;
	vector<ImageReflection> reflections;

	ReflectionManager(void);
	~ReflectionManager(void);

	void addHolderForImage(Holder *holder, MtzManager *manager, double l);
	double l_sum_E_alpha_2(vector<Scale_factor> Gs);
	double contributionToGradient(vector<Scale_factor> Gs, int l);
	void sortReflections();
	
	double psi_contribution(vector<Scale_factor> Gs);
	double newGradientContribution(vector<Scale_factor> Gs, int l);
	
	double getCorrectedIntensity(vector<Scale_factor> Gs);
	double getWeightingTerm(int exclude_first);
	double getCorrectedIntensityDerivative(vector<Scale_factor> Gs, int l);
	double averageScaleFactor(vector<Scale_factor> Gs);
	double rMerge(vector<Scale_factor> *Gs, double *numContribution, double *denomContribution, double resolution);
    void splitIntensities(vector<Scale_factor> *Gs,
                          double *int1, double *int2);
    double intensity(vector<Scale_factor> *Gs, double *sigma);
    Holder *mergedHolder(vector<Scale_factor> *Gs, bool half, bool all);
	
	double E_alpha(vector<Scale_factor> Gs, int l, double *sum_alpha_beta, double *sum_alpha_alpha);
    
    void setGroup(CCP4SPG *newGroup)
    {
        group = newGroup;
    }
};

#endif

