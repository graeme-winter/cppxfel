/*
 * MtzRefiner.h
 *
 *  Created on: 4 Jan 2015
 *      Author: helenginn
 */

#ifndef MTZREFINER_H_
#define MTZREFINER_H_

#include "parameters.h"
#include "MtzManager.h"
#include "PanelParser.h"

class MtzRefiner
{
private:
    vector<MtzPtr> mtzManagers;
	MtzManager *reference;
	vector<Image *> images;
    static bool hasPanelParser;
    PanelParser *panelParser;
    static int imageLimit;
    static int imageMax(size_t lineCount);
    static void singleLoadImages(std::string *filename, vector<Image *> *newImages, int offset);
    static void readSingleImageV2(std::string *filename, vector<Image *> *newImages, vector<MtzPtr> *newMtzs, int offset);
    void indexImage(int offset, vector<MtzPtr> *mtzSubset);
    static void indexImageWrapper(MtzRefiner *object, int offset, vector<MtzPtr> *mtzSubset);
    void applyParametersToImages();
    static int cycleNum;
    bool hasRefined;
public:
	MtzRefiner();
	virtual ~MtzRefiner();

    void index();
	bool loadInitialMtz(bool force = false);

	void cycle();
	void cycleThread(int offset);
	static void cycleThreadWrapper(MtzRefiner *object, int offset);

    void refineSymmetry();
	void refine();
	void refineCycle(bool once = false);
	void readMatricesAndMtzs();
    void refineDetectorGeometry();
    void refineMetrology();
    void loadMillersIntoPanels();
    void plotDetectorGains();
    void initialMerge();
    void orientationPlot();
    void applyUnrefinedPartiality();

    void loadPanels();
	void integrate(bool orientation);
	static void integrateImagesWrapper(MtzRefiner *object,
			vector<MtzPtr> *&mtzSubset, int offset, bool orientation);
	void integrateImages(vector<MtzPtr> *&mtzSubset, int offset, bool orientation);
	void readMatricesAndImages(std::string *filename = NULL, bool areImages = true);

	static void readMatrix(double (&matrix)[9], std::string line);
	static void singleThreadRead(vector<std::string> lines,
			vector<MtzPtr> *mtzManagers, int offset);
	void merge();
    void correlationAndInverse(bool shouldFlip = false);
    
    static int getCycleNum()
    {
        return cycleNum;
    }
    
    vector<MtzPtr> getMtzManagers()
    {
        return mtzManagers;
    }
    
    void refineDistances();
    void polarisationGraph();
    void displayIndexingHands();
    void findSteps();
    
    void writeNewOrientations(bool includeRots = false, bool detailed = false);
    void removeSigmaValues();
    void readXFiles(std::string filename);
    void xFiles();
};

#endif /* MTZREFINER_H_ */
