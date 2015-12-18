/*
 * Spot.h
 *
 *  Created on: 30 Dec 2014
 *      Author: helenginn
 */

#ifndef SPOT_H_
#define SPOT_H_

#include <vector>
#include "parameters.h"
#include "Panel.h"
#include "Vector.h"
#include "LoggableObject.h"

class Image;

class Spot : LoggableObject
{
private:
	vector<vector<double> > probe;
	ImageWeakPtr parentImage;
    double angleDetectorPlane;
    bool setAngle;
    bool checked;
    int successfulCommonLines;
    double correctedX; double correctedY;
    double x; double y;
    bool rejected;
    
public:
	Spot(ImagePtr image);
	virtual ~Spot();

    double weight();
	double maximumLift(ImagePtr image, int x, int y, bool ignoreCovers);
	double maximumLift(ImagePtr image, int x, int y);
	void makeProbe(int height, int size);
	void setXY(int x, int y);
	double scatteringAngle(ImagePtr image = NULL);
	bool isAcceptable(ImagePtr image);
	static void sortSpots(vector<Spot *> *spots);
	static bool spotComparison(Spot *a, Spot *b);
    double angleFromSpotToCentre(double centreX, double centreY);
    double angleInPlaneOfDetector(double centreX = 0, double centreY = 0, vec upBeam = new_vector(0, 1, 0));
    bool isOnSameLineAsSpot(SpotPtr spot2, double tolerance);
    double resolution();
    static void writeDatFromSpots(std::string filename, std::vector<SpotPtr> spots);
    
    Coord getXY();
    double getX(bool update = false);
    double getY(bool update = false);
    Coord getRawXY();
    vec estimatedVector();
    void setUpdate();
    
    std::string spotLine();
    
    void setRejected(bool isRejected = true)
    {
        rejected = isRejected;
    }
    
    bool isRejected()
    {
        return rejected;
    }
    
    int successfulLineCount()
    {
        return successfulCommonLines;
    }
    
    void addSuccessfulCommonLineCount()
    {
        successfulCommonLines++;
    }
    
    void deleteSuccessfulCommonLineCount()
    {
        successfulCommonLines--;
    }
    
    bool isChecked()
    {
        return checked;
    }
    
    void setChecked(bool newCheck = true)
    {
        checked = newCheck;
    }
    
    ImagePtr getParentImage()
	{
		return parentImage.lock();
	}

	void setParentImage(ImagePtr parentImage)
	{
		this->parentImage = parentImage;
	}
    

};

#endif /* SPOT_H_ */
