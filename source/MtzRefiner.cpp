/*
 * MtzRefiner.cpp
 *
 *  Created on: 4 Jan 2015
 *      Author: helenginn
 */

#include "MtzRefiner.h"
#include <vector>
#include "misc.h"
#include "Vector.h"
#include <boost/thread/thread.hpp>
#include "GraphDrawer.h"
#include "Hdf5Image.h"
#include "Image.h"
#include "Miller.h"
#include <fstream>
#include "AmbiguityBreaker.h"
#include "IndexManager.h"
#include "FreeMillerLibrary.h"

#include "FileParser.h"
#include "parameters.h"
#include "FileReader.h"

#include "Logger.h"
#include "MtzGrouper.h"
#include "MtzMerger.h"
#include "Hdf5ManagerProcessing.h"
#include "Detector.h"
#include "GeometryParser.h"
#include "GeometryRefiner.h"

int MtzRefiner::imageLimit;
int MtzRefiner::cycleNum;



MtzRefiner::MtzRefiner()
{
    // TODO Auto-generated constructor stub
    int logInt = FileParser::getKey("VERBOSITY_LEVEL", 0);
    Logger::mainLogger->changePriorityLevel((LogLevel)logInt);
    
    imageLimit = FileParser::getKey("IMAGE_LIMIT", 0);
    
    hasRefined = false;
    isPython = false;
    readRefinedMtzs = FileParser::getKey("READ_REFINED_MTZS", false);
    
    indexManager = NULL;
}

// MARK: Refinement

void MtzRefiner::cycleThreadWrapper(MtzRefiner *object, int offset)
{
    object->cycleThread(offset);
}

void MtzRefiner::cycleThread(int offset)
{
    int img_num = (int)mtzManagers.size();
    int j = 0;
    
    bool partialitySpectrumRefinement = FileParser::getKey("REFINE_ENERGY_SPECTRUM", false);
    
    std::vector<int> targets = FileParser::getKey("TARGET_FUNCTIONS", std::vector<int>());
    bool lowMemoryMode = FileParser::getKey("LOW_MEMORY_MODE", false);
    
    int maxThreads = FileParser::getMaxThreads();
    
    for (int i = offset; i < img_num; i += maxThreads)
    {
        j++;
        
        std::ostringstream logged;
        
        MtzPtr image = mtzManagers[i];
        
        if (!image->isRejected())
        {
            logged << "Refining image " << i << " " << image->getFilename() << std::endl;
            Logger::mainLogger->addStream(&logged);
            
            bool silent = (targets.size() > 0);
            
            if (partialitySpectrumRefinement)
            {
                image->refinePartialities();
                //        image->replaceBeamWithSpectrum();
            }
            else
            {
                image->loadReflections(true);
                image->gridSearch(silent);
                
                if (lowMemoryMode)
                {
                    image->dropReflections();
                }
            }
            
            if (targets.size() > 0)
            {
                ScoreType firstScore = image->getScoreType();
                for (int i = 0; i < targets.size(); i++)
                {
                    silent = (i < targets.size() - 1);
                    image->setDefaultScoreType((ScoreType)targets[i]);
                    
                    if (partialitySpectrumRefinement)
                    {
                        image->refinePartialities();
                    }
                    else
                    {
                        image->gridSearch(silent);
                    }
                }
                image->setDefaultScoreType(firstScore);
            }
            
            //    image->writeToDat();
        }
    }
}

void MtzRefiner::cycle()
{
    MtzManager::setReference(&*reference);
    
    time_t startcputime;
    time(&startcputime);
    
    boost::thread_group threads;
    
    int maxThreads = FileParser::getMaxThreads();
    
    std::ostringstream logged;
    logged << "Filename\tScore type\t\tCorrel\tRfactor\tPart correl\tRmerge\tB factor\tHits" << std::endl;
    Logger::mainLogger->addStream(&logged);
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(cycleThreadWrapper, this, i);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    time_t endcputime;
    time(&endcputime);
    
    clock_t difference = endcputime - startcputime;
    double seconds = difference;
    
    int finalSeconds = (int) seconds % 60;
    int minutes = seconds / 60;
    
    std::cout << "N: Refinement cycle: " << minutes << " minutes, "
    << finalSeconds << " seconds." << std::endl;
}

void MtzRefiner::initialMerge()
{
    MtzManager *originalMerge = NULL;
    
    AmbiguityBreaker breaker = AmbiguityBreaker(mtzManagers);
    breaker.run();
    referencePtr = breaker.getMergedMtz();
    originalMerge = &*referencePtr;
    reference = referencePtr;
    
    reference->writeToFile("initialMerge.mtz");
}

void MtzRefiner::setupFreeMillers()
{
    std::string freeMiller = FileParser::getKey("FREE_MILLER_LIST", std::string(""));
    double freeMillerProportion = FileParser::getKey("FREE_MILLER_PROPORTION", 0.0);
    
    if (freeMiller.length() && freeMillerProportion > 0)
    {
        FreeMillerLibrary::setup();
    }
}

void MtzRefiner::refine()
{
    setupFreeMillers();
    
    MtzPtr originalMerge;
    
    bool initialExists = loadInitialMtz();
    readMatricesAndMtzs();
    
    if (!initialExists)
    {
        initialMerge();
    }
    
    bool fixUnitCell = FileParser::getKey("FIX_UNIT_CELL", false);
    
    if (fixUnitCell)
    {
        for (int i = 0; i < mtzManagers.size(); i++)
        {
            vector<double> givenUnitCell = FileParser::getKey("UNIT_CELL", vector<double>());
            
            mtzManagers[i]->getMatrix()->changeOrientationMatrixDimensions(givenUnitCell[0], givenUnitCell[1], givenUnitCell[2], givenUnitCell[3], givenUnitCell[4], givenUnitCell[5]);
            mtzManagers[i]->setUnitCell(givenUnitCell);
        }
    }
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        mtzManagers[i]->loadParametersMap();
    }
    
    logged << "N: Total crystals loaded: " << mtzManagers.size() << std::endl;
    sendLog();
    
    reference->writeToFile("originalMerge.mtz");
    
    MtzManager::setReference(&*reference);
    
    refineCycle();
    
    hasRefined = true;
}

void MtzRefiner::refineCycle(bool once)
{
    int i = 0;
    bool finished = false;
    
    int minimumCycles = FileParser::getKey("MINIMUM_CYCLES", 6);
    int maximumCycles = FileParser::getKey("MAXIMUM_CYCLES", 50);
    
    bool stop = FileParser::getKey("STOP_REFINEMENT", true);
    double correlationThreshold = FileParser::getKey("CORRELATION_THRESHOLD",
                                                     CORRELATION_THRESHOLD);
    double initialCorrelationThreshold = FileParser::getKey(
                                                            "INITIAL_CORRELATION_THRESHOLD", INITIAL_CORRELATION_THRESHOLD);
    int thresholdSwap = FileParser::getKey("THRESHOLD_SWAP",
                                           THRESHOLD_SWAP);
    bool exclusion = FileParser::getKey("EXCLUDE_OWN_REFLECTIONS", false);
    bool replaceReference = FileParser::getKey("REPLACE_REFERENCE", true);
    
    double resolution = FileParser::getKey("MAX_REFINED_RESOLUTION",
                                           MAX_OPTIMISATION_RESOLUTION);
    int scalingInt = FileParser::getKey("SCALING_STRATEGY",
                                        (int) SCALING_STRATEGY);
    ScalingType scaling = (ScalingType) scalingInt;
    
    bool anomalousMerge = FileParser::getKey("MERGE_ANOMALOUS", false);
    bool outputIndividualCycles = FileParser::getKey("OUTPUT_INDIVIDUAL_CYCLES", false);
    bool old = FileParser::getKey("OLD_MERGE", true);
    
    while (!finished)
    {
        if (outputIndividualCycles)
        {
            FileParser::setKey("CYCLE_NUMBER", i);
        }
        
        cycleNum = i;
        cycle();
        
        // *************************
        // ***** NORMAL MERGE ******
        // *************************
        
        MtzPtr mergedMtz;
        std::string filename = "allMerge" + i_to_str(i) + ".mtz";
        
        if (!old)
        {
            MtzManager::setReference(&*reference);
            
            MtzMerger merger;
            merger.setAllMtzs(mtzManagers);
            merger.setCycle(cycleNum);
            merger.setScalingType(scaling);
            merger.mergeFull();
            mergedMtz = merger.getMergedMtz();
            
            referencePtr = mergedMtz;
            
            if (anomalousMerge)
            {
                merger.mergeFull(true);
            }
            
            merger.setFreeOnly(true);
            merger.mergeFull();
            
            std::string filename = merger.getFilename();
            
            if (replaceReference)
            {
                reference = mergedMtz;
                MtzManager::setReference(&*reference);
            }
        }
        else
        {
            
            std::cout << "Grouping final MTZs" << std::endl;
            MtzGrouper *grouper = new MtzGrouper();
            MtzManager::setReference(&*reference);
            if (i >= 0)
                grouper->setScalingType(scaling);
            if (once)
                grouper->setScalingType(ScalingTypeAverage);
            grouper->setWeighting(WeightTypePartialitySigma);
            grouper->setExpectedResolution(resolution);
            grouper->setMtzManagers(mtzManagers);
            grouper->setExcludeWorst(true);
            if (i < thresholdSwap)
                grouper->setCorrelationThreshold(initialCorrelationThreshold);
            else
                grouper->setCorrelationThreshold(correlationThreshold);
            
            MtzPtr mergedMtz, unmergedMtz;
            grouper->merge(&mergedMtz, &unmergedMtz, i);
            
            std::cout << "Reflections: " << mergedMtz->reflectionCount() << std::endl;
            if (!once)
            {
                double correl = mergedMtz->correlation(true);
                std::cout << "N: Merged correlation = " << correl << std::endl;
            }
            mergedMtz->description();
            
            double scale = 1000 / mergedMtz->averageIntensity();
            mergedMtz->applyScaleFactor(scale);
            
            mergedMtz->writeToFile(filename.c_str(), true);
            
            
            // *************************
            // ******* ANOMALOUS *******
            // *************************
            
            if (anomalousMerge)
            {
                MtzPtr anomMergedMtz;
                grouper->merge(&anomMergedMtz, NULL, i, true);
                
                MtzManager::setReference(&*reference);
                
                std::cout << "Reflections for anomalous: " << anomMergedMtz->reflectionCount() << std::endl;
                
                double scale = 1000 / mergedMtz->averageIntensity();
                mergedMtz->applyScaleFactor(scale);
            }
            
            
            if (replaceReference)
            {
                reference = mergedMtz;
                MtzManager::setReference(&*reference);
            }
        }
        
        /*
        MtzManager *reloadMerged = new MtzManager();
        std::string reloadFullPath = FileReader::addOutputDirectory(filename);
        
        reloadMerged->setFilename(reloadFullPath.c_str());
        reloadMerged->loadReflections(1);
        reloadMerged->description();
        
        double reloaded = reloadMerged->correlation(true);
        std::cout << "Reloaded correlation = " << reloaded << std::endl;
        
        if (reloaded > 0.999 && i >= minimumCycles && stop)
            finished = true;*/
        
        if (once)
            finished = true;
        
        if (i == maximumCycles - 1 && stop)
            finished = true;
        
        i++;
        //     delete grouper;
    }
    
}


// MARK: Loading data

bool MtzRefiner::loadInitialMtz(bool force)
{
    bool hasInitialMtz = FileParser::hasKey("INITIAL_MTZ");
    
    if (reference && !force)
        return true;
    
    logged << "Initial MTZ has "
    << (hasInitialMtz ? "" : "not ") << "been provided." << std::endl;
    sendLog();
    
    if (hasInitialMtz)
    {
        std::string referenceFile = FileParser::getKey("INITIAL_MTZ",
                                                       std::string(""));
        
        reference = MtzPtr(new MtzManager());
        
        reference->setFilename(referenceFile.c_str());
        reference->loadReflections(1);
        reference->setSigmaToUnity();
        
        if (reference->reflectionCount() == 0)
        {
            logged << "Initial MTZ reference missing or reflection count is 0. Exiting." << std::endl;
            sendLogAndExit();
        }
        
        MtzManager::setReference(&*reference);
    }
    
    
    return hasInitialMtz;
}


int MtzRefiner::imageMax(size_t lineCount)
{
    int skip = imageSkip(lineCount);
    
    int end = (int)lineCount;
    
    if (imageLimit != 0)
        end = imageLimit < lineCount ? imageLimit : (int)lineCount;
    
    end += skip;
    
    if (end > lineCount)
        end = (int)lineCount;
    
    return end;
}

void MtzRefiner::readSingleImageV2(std::string *filename, vector<ImagePtr> *newImages, vector<MtzPtr> *newMtzs, int offset, bool v3, MtzRefiner *me)
{
    double wavelength = FileParser::getKey("INTEGRATION_WAVELENGTH", 0.0);
    double detectorDistance = FileParser::getKey("DETECTOR_DISTANCE", 0.0);
    
    Image::setGlobalDetectorDistance(NULL, detectorDistance);
    
    vector<double> givenUnitCell = FileParser::getKey("UNIT_CELL", vector<double>());
    
    std::vector<std::string> hdf5Sources = FileParser::getKey("HDF5_SOURCE_FILES", std::vector<std::string>());
    bool readFromHdf5 = hdf5Sources.size() > 0;
    bool fixUnitCell = FileParser::getKey("FIX_UNIT_CELL", false);
    vector<double> cellDims = FileParser::getKey("UNIT_CELL", vector<double>());
    
    bool checkingUnitCell = false;
    
    bool hasBeamCentre = FileParser::hasKey("BEAM_CENTRE");
    
    bool setSigmaToUnity = FileParser::getKey("SET_SIGMA_TO_UNITY", true);
    
    bool ignoreMissing = FileParser::getKey("IGNORE_MISSING_IMAGES", false);
    bool lowMemoryMode = FileParser::getKey("LOW_MEMORY_MODE", false);
    
    if (readFromHdf5)
    {
        ignoreMissing = true;
    }
    
    const std::string contents = FileReader::get_file_contents( filename->c_str());
    vector<std::string> imageList = FileReader::split(contents, "\nimage ");
    
    int maxThreads = FileParser::getMaxThreads();
    
    int skip = imageSkip(imageList.size());
    int end = imageMax(imageList.size());
    
    if (skip > 0)
    {
        std::ostringstream logged;
        logged << "Skipping " << skip << " lines" << std::endl;
        Logger::mainLogger->addStream(&logged);
    }
    
    for (int i = offset + skip; i < end; i += maxThreads)
    {
        if (imageList[i].length() == 0)
            continue;
        
        vector<std::string> lines = FileReader::split(imageList[i], '\n');
        
        vector<std::string> components = FileReader::split(lines[0], ' ');
        
        if (components.size() <= 1)
            continue;
        
        std::string imgName = components[1];
        std::string imgNameOnly = components[1];
        
        imgName.erase(std::remove(imgName.begin(), imgName.end(), '\r'), imgName.end());
        imgName.erase(std::remove(imgName.begin(), imgName.end(), '\n'), imgName.end());
        
        if (newImages)
            imgName += ".img";
        else if (newMtzs)
            imgName += ".mtz";
        
        std::ostringstream logged;
        
        if (!FileReader::exists(imgName) && !ignoreMissing && !v3)
        {
            logged << "Skipping image " << imgName << std::endl;
            Logger::mainLogger->addStream(&logged);
            continue;
        }
        
        if ((readFromHdf5 && newImages != NULL) || v3)
        {
            Hdf5ManagerCheetahPtr manager = Hdf5ManagerCheetah::hdf5ManagerForImage(imgNameOnly);
            
            if (!manager)
            {
                continue;
            }
        }
        
        double usedWavelength = wavelength;
        double usedDistance = detectorDistance;
        
        bool fromDials = FileParser::getKey("FROM_DIALS", true);
        
        if (components.size() >= 3)
        {
            usedWavelength = atof(components[1].c_str());
            usedDistance = atof(components[2].c_str());
        }
        
        MatrixPtr unitCell;
        MatrixPtr newMatrix;
        double delay = 0;
        std::string paramsLine = "";
        
        ImagePtr newImage;
        
        if (readFromHdf5)
        {
            Hdf5ImagePtr hdf5Image = Hdf5ImagePtr(new Hdf5Image(imgName, wavelength,
                                                                0));
            newImage = boost::static_pointer_cast<Image>(hdf5Image);
        }
        else
        {
            newImage = ImagePtr(new Image(imgName, wavelength,
                                          0));
        }
        
        bool hasSpots = false;
        std::string parentImage = "";
        int currentCrystal = -1;
        
        for (int i = 1; i < lines.size(); i++)
        {
            vector<std::string> components = FileReader::split(lines[i], ' ');
            
            if (components.size() == 0)
                continue;
            
            if (components[0] == "spots")
            {
                std::string spotsFile = components[1];
                newImage->setSpotsFile(spotsFile);
                hasSpots = true;
            }
            
            if (components[0] == "parent")
            {
                parentImage = components[1];
            }
            
            if (components[0] == "crystal")
            {
                currentCrystal = atoi(components[1].c_str());
            }
            
            if (components[0] == "matrix")
            {
                // individual matrices
                double matrix[9];
                readMatrix(matrix, lines[i]);
                
                newMatrix = MatrixPtr(new Matrix(matrix));
                
                if (fromDials)
                {
                    newMatrix->rotate(0, 0, M_PI / 2);
                }
                
                if (newImages)
                {
                    newImage->setUpIOMRefiner(newMatrix);
                }
            }
            
            if (components[0] == "wavelength")
            {
                double newWavelength = wavelength;
                
                if (components.size() >= 2)
                    newWavelength = atof(components[1].c_str());
                
                newImage->setWavelength(newWavelength);
                
                logged << "Setting wavelength for " << imgName << " to " << newWavelength << " Angstroms." << std::endl;
                Logger::log(logged);
            }
            
            if (components[0] == "distance")
            {
                double newDistance = detectorDistance;
                
                if (components.size() >= 2)
                    newDistance = atof(components[1].c_str());
                
                newImage->setDetectorDistance(newDistance);
            }
            
            
            if (components[0] == "centre" && !hasBeamCentre)
            {
                if (components.size() < 3)
                    continue;
                
                double centreX = atof(components[1].c_str());
                double centreY = atof(components[2].c_str());
                
                newImage->setBeamX(centreX);
                newImage->setBeamY(centreY);
            }
            
            if (components[0] == "unitcell")
            {
                // individual matrices
                double matrix[9];
                readMatrix(matrix, lines[i]);
                
                unitCell = MatrixPtr(new Matrix(matrix));
            }
            
            if (components[0] == "rotation")
            {
                // individual matrices
                double matrix[9];
                readMatrix(matrix, lines[i]);
                
                MatrixPtr rotation = MatrixPtr(new Matrix(matrix));
                
                if (unitCell)
                {
                    if (newImages)
                    {
                        vector<double> correction = FileParser::getKey("ORIENTATION_CORRECTION", vector<double>());
                        
                        if (fromDials && !v3)
                        {
                            rotation->rotate(0, 0, M_PI / 2);
                        }
                        
                        if (correction.size() >= 2)
                        {
                            double rightRot = correction[0] * M_PI / 180;
                            double upRot = correction[1] * M_PI / 180;
                            double swivelRot = 0;
                            
                            if (correction.size() > 2)
                                swivelRot = correction[2] * M_PI / 180;
                            
                            rotation->rotate(rightRot, upRot, swivelRot);
                        }
                    }
                    
                    newMatrix = MatrixPtr(new Matrix);
                    newMatrix->setComplexMatrix(unitCell, rotation);
                    
                    if (fixUnitCell)
                    {
                        newMatrix->changeOrientationMatrixDimensions(cellDims[0], cellDims[1], cellDims[2], cellDims[3], cellDims[4], cellDims[5]);
                    }
                    
                    if (newImages)
                    {
                        newImage->setUpIOMRefiner(newMatrix);
                    }
                    
                    if (v3 && newMtzs)
                    {
                        MtzPtr newManager = MtzPtr(new MtzManager());
                        std::string prefix = (me->readRefinedMtzs ? "ref-" : "");
                        newManager->setFilename((prefix + "img-" + imgNameOnly + "_" + i_to_str(currentCrystal) + ".mtz").c_str());
                        newManager->setMatrix(newMatrix);
                        
                        if (setSigmaToUnity)
                            newManager->setSigmaToUnity();
                        
                        newManager->setParamLine(paramsLine);
                        newManager->setTimeDelay(delay);
                        newManager->setImage(newImage);
                        newManager->loadReflections(PartialityModelScaled);
                        
                        if (newManager->reflectionCount() > 0)
                        {
                            newImage->addMtz(newManager);
                            newMtzs->push_back(newManager);
                        }
                    }
                    
                    unitCell = MatrixPtr();
                }
                else
                {
                    std::cout << "Warning, unmatched unitcell / rotation pair?" << std::endl;
                }
            }
            
            if (components[0] == "params" && newMtzs)
            {
                paramsLine = lines[i];
            }
            
            if (components[0] == "delay" && newMtzs)
            {
                if (components.size() > 1)
                    delay = atof(components[1].c_str());
            }
        }
        
        if (newImages)
        {
/*            if (hasSpots)
            {
                newImage->processSpotList();
            }*/
            
            newImages->push_back(newImage);
        }
        
        if (newMtzs && !v3)
        {
            MtzPtr newManager = MtzPtr(new MtzManager());
            
            newManager->setFilename(imgName.c_str());
            
            newManager->setParentImageName(parentImage);
            
            if (!newMatrix)
            {
                logged << "Warning! Matrix for " << imgName << " is missing." << std::endl;
                Logger::setShouldExit();
                Logger::log(logged);
            }
            
            newManager->setMatrix(newMatrix);
            
            if (!lowMemoryMode)
            {
                newManager->loadReflections(PartialityModelScaled, true);
            }
            
            if (setSigmaToUnity)
                newManager->setSigmaToUnity();
            
            newManager->setParamLine(paramsLine);
            newManager->setTimeDelay(delay);
            
            if (newManager->reflectionCount() > 0 || lowMemoryMode)
            {
                newMtzs->push_back(newManager);
            }
        }
    }
}

void MtzRefiner::readDataFromOrientationMatrixList(std::string *filename, bool areImages, std::vector<ImagePtr> *targetImages)
{
    double version = FileParser::getKey("MATRIX_LIST_VERSION", 2.0);
    
    // thought: turn the vector concatenation into a templated function
    
    boost::thread_group threads;
    
    int maxThreads = FileParser::getMaxThreads();
    
    vector<vector<ImagePtr> > imageSubsets;
    vector<vector<MtzPtr> > mtzSubsets;
    imageSubsets.resize(maxThreads);
    mtzSubsets.resize(maxThreads);
    
    std::string contents;
    
    try
    {
        contents = FileReader::get_file_contents( filename->c_str());
    }
    catch (int errno)
    {
        logged << "Missing file " << filename << ", cannot continue." << std::endl;
        sendLogAndExit();
    }
    vector<std::string> imageList = FileReader::split(contents, "\nimage ");
    
    std::ostringstream logged;
    
    if (imageList[0].substr(0, 6) == "ersion")
    {
        std::string vString = imageList[0].substr(7, imageList[0].length() - 7);
        float inputVersion = atof(vString.c_str());
        
        logged << "Autodetecting matrix list version: " << inputVersion << std::endl;
        
        version = inputVersion;
    }
    
    Logger::log(logged);
    
    if (version > 2.99 && version < 3.99)
    {
        loadPanels();
    }

    for (int i = 0; i < maxThreads; i++)
    {
        if (version <= 1.1) // floating point comparison???
        {
            boost::thread *thr = new boost::thread(singleLoadImages, filename,
                                                   &imageSubsets[i], i);
            threads.add_thread(thr);
        }
        else if (version > 1.99 && version < 2.99)
        {
            vector<MtzPtr> *chosenMtzs = areImages ? NULL : &mtzSubsets[i];
            vector<ImagePtr> *chosenImages = areImages ? &imageSubsets[i] : NULL;
            boost::thread *thr = new boost::thread(readSingleImageV2, filename,
                                                   chosenImages, chosenMtzs, i, false, this);
            threads.add_thread(thr);
        }
        else if (version > 2.99 && version < 3.99)
        {
            boost::thread *thr = new boost::thread(readSingleImageV2, filename,
                                                   &imageSubsets[i], &mtzSubsets[i], i, true, this);
            threads.add_thread(thr);
        }
    }
    
    threads.join_all();
    
    int total = 0;
    
    for (int i = 0; i < maxThreads; i++)
    {
        if (areImages)
            total += imageSubsets[i].size();
        
        if (!areImages)
            total += mtzSubsets[i].size();
    }
    
    if (targetImages == NULL)
    {
        targetImages = &images;
    }
    
    mtzManagers.reserve(total);
    targetImages->reserve(total);
    int lastPos = 0;
    
    for (int i = 0; i < maxThreads; i++)
    {
        if (areImages)
        {
            targetImages->insert(targetImages->begin() + lastPos,
                                 imageSubsets[i].begin(), imageSubsets[i].end());
            lastPos += imageSubsets[i].size();
        }
        else
        {
            mtzManagers.insert(mtzManagers.begin() + lastPos,
                               mtzSubsets[i].begin(), mtzSubsets[i].end());
            lastPos += mtzSubsets[i].size();
        }
    }
}

void MtzRefiner::readMatricesAndImages(std::string *filename, bool areImages, std::vector<ImagePtr> *targetImages)
{
    if (targetImages == NULL && images.size() > 0)
        return;
    
    std::string hdf5OutputFile = FileParser::getKey("HDF5_OUTPUT_FILE", std::string(""));
    std::vector<std::string> hdf5ProcessingFiles = FileParser::getKey("HDF5_SOURCE_FILES", std::vector<std::string>());
    
    bool hdf5 = hdf5OutputFile.length() || hdf5ProcessingFiles.size();
    Hdf5ManagerCheetah::initialiseCheetahManagers();

    std::string aFilename = "";
    aFilename = FileParser::getKey("ORIENTATION_MATRIX_LIST", std::string(""));
    
    if (!hdf5)
    {
        if (aFilename.length() == 0)
        {
            logged << "No orientation matrix list provided (and no HDF5 file either). Exiting now." << std::endl;
            sendLogAndExit();
        }
    }
    
    if (aFilename.length() && !FileReader::exists(aFilename))
    {
        std::ostringstream logged;
        logged << "File specified in ORIENTATION_MATRIX_LIST " << aFilename << " doesn't exist." << std::endl;
        
        if (hdf5)
        {
            logged << "You have specified HDF5 image files, so you don't necessarily need to specify your ORIENTATION_MATRIX_LIST." << std::endl;
        }
        else
        {
            logged << "You must either specify some HDF5 image files using HDF5_SOURCE_FILES, or you must specify ORIENTATION_MATRIX_LIST to load .img files from the file system instead." << std::endl;
        }
        
        LoggableObject::staticLogAndExit(logged);
    }
    
    if (aFilename.length())
    {
        readDataFromOrientationMatrixList(&aFilename, areImages, targetImages);
    }
    
    if (hdf5)
    {
        readFromHdf5(&images);
    }
    
    if (hdf5)
    {
        Hdf5ImagePtr maskImage = Hdf5ImagePtr(new Hdf5Image());
        maskImage->setMask();
    }

    
    logged << "Summary\nImages: " << images.size() << "\nCrystals: " << mtzManagers.size() << std::endl;
    sendLog();
}

void MtzRefiner::singleLoadImages(std::string *filename, vector<ImagePtr> *newImages, int offset)
{
    const std::string contents = FileReader::get_file_contents(
                                                               filename->c_str());
    
    vector<std::string> lines = FileReader::split(contents, '\n');
    
    int spg_num = FileParser::getKey("SPACE_GROUP", -1);
    
    if (spg_num == -1)
    {
        std::cout << "Please set space group number under keyword SPACE_GROUP"
        << std::endl;
    }
    
    double overPredSpotSize = FileParser::getKey("OVER_PRED_RLP_SIZE",
                                                 OVER_PRED_SPOT_SIZE);
    double overPredBandwidth = FileParser::getKey("OVER_PRED_BANDWIDTH",
                                                  OVER_PRED_BANDWIDTH);
    overPredBandwidth /= 2;
    
    double orientationStep = FileParser::getKey("INITIAL_ORIENTATION_STEP", INITIAL_ORIENTATION_STEP);
    
    // @TODO intensityThreshold should be flexible
    double intensityThreshold = FileParser::getKey("INTENSITY_THRESHOLD",
                                                   INTENSITY_THRESHOLD);
    
    double metrologySearchSize = FileParser::getKey("METROLOGY_SEARCH_SIZE",
                                                    METROLOGY_SEARCH_SIZE);
    
    double maxIntegratedResolution = FileParser::getKey(
                                                        "MAX_INTEGRATED_RESOLUTION", MAX_INTEGRATED_RESOLUTION);
    
    double wavelength = FileParser::getKey("INTEGRATION_WAVELENGTH", 0.0);
    double detectorDistance = FileParser::getKey("DETECTOR_DISTANCE", 0.0);
    
    bool fixUnitCell = FileParser::getKey("FIX_UNIT_CELL", true);
    std::ostringstream logged;
    
    if (wavelength == 0)
    {
        logged
        << "Please provide initial wavelength for integration under keyword INTEGRATION_WAVELENGTH"
        << std::endl;
        Logger::mainLogger->addStream(&logged, LogLevelNormal);
    }
    
    if (detectorDistance == 0)
    {
        logged
        << "Please provide detector distance for integration under keyword DETECTOR_DISTANCE"
        << std::endl;
        Logger::mainLogger->addStream(&logged, LogLevelNormal);
    }
    
    vector<double> unitCell = FileParser::getKey("UNIT_CELL", vector<double>());
    
    if (unitCell.size() == 0)
    {
        logged
        << "Please provide unit cell dimensions under keyword UNIT_CELL"
        << std::endl;
        Logger::mainLogger->addStream(&logged, LogLevelNormal);
        
        fixUnitCell = false;
    }
    
    int maxThreads = FileParser::getMaxThreads();
    
    int end = imageMax(lines.size());
    
    int skip = imageSkip(lines.size());
    
    if (skip > 0)
    {
        std::cout << "Skipping " << skip << " lines" << std::endl;
    }
    
    for (int i = offset + skip; i < end + skip; i += maxThreads)
    {
        vector<std::string> components = FileReader::split(lines[i], ' ');
        
        if (components.size() == 0)
            continue;
        
        std::string imgName = components[0] + ".img";
        if (!FileReader::exists(imgName))
        {
            continue;
        }
        
        if (components.size() > 11)
        {
            wavelength = atof(components[10].c_str());
            detectorDistance = atof(components[11].c_str());
        }
        
        
        ImagePtr newImage = ImagePtr(new Image(imgName, wavelength,
                                               0));
        
        MatrixPtr newMat = MatrixPtr();
        
        if (components.size() >= 10)
        {
            double matrix[9];
            readMatrix(matrix, lines[i]);
            newMat = MatrixPtr(new Matrix(matrix));
            if (fixUnitCell)
                newMat->changeOrientationMatrixDimensions(unitCell[0], unitCell[1], unitCell[2], unitCell[3], unitCell[4], unitCell[5]);
        }
        else
        {
            newMat = Matrix::matrixFromUnitCell(&unitCell[0]);
        }
        
        newImage->setUpIOMRefiner(newMat);
        newImage->setPinPoint(true);
        
        CCP4SPG *spg = ccp4spg_load_by_standard_num(spg_num);
        
        newImage->setSpaceGroup(spg);
        newImage->setMaxResolution(maxIntegratedResolution);
        newImage->setSearchSize(metrologySearchSize);
        newImage->setUnitCell(unitCell);
        newImage->setInitialStep(orientationStep);
        
        newImage->setTestSpotSize(overPredSpotSize);
        newImage->setTestBandwidth(overPredBandwidth);
        
        newImages->push_back(newImage);
    }
}

void MtzRefiner::readFromHdf5(std::vector<ImagePtr> *newImages)
{
    // in the case where HDF5_OUTPUT_FILE is set.
    double wavelength = FileParser::getKey("INTEGRATION_WAVELENGTH", 0.0);
    double detectorDistance = FileParser::getKey("DETECTOR_DISTANCE", 0.0);
    std::vector<double> beamCentre = FileParser::getKey("BEAM_CENTRE", std::vector<double>(0, 2));
    vector<double> givenUnitCell = FileParser::getKey("UNIT_CELL", vector<double>());
    
    int count = 0;
    
    std::string orientationList = FileParser::getKey("ORIENTATION_MATRIX_LIST", std::string(""));
    bool hasList = (orientationList.length() > 0);
    
    std::string hdf5OutputFile = FileParser::getKey("HDF5_OUTPUT_FILE", std::string(""));

    if (hdf5OutputFile.length())
    {
        hdf5ProcessingPtr = Hdf5ManagerProcessingPtr(new Hdf5ManagerProcessing(hdf5OutputFile));
    }
    
    int startImage = FileParser::getKey("IMAGE_SKIP", 0);
    int endImage = startImage + FileParser::getKey("IMAGE_LIMIT", 0);
    
    int loadAll = (startImage == 0 && endImage == 0);
    
    if (!loadAll)
    {
        logged << "Starting from image " << startImage << " and ending on image " << endImage << std::endl;
    }
    else
    {
        logged << "Loading all images from HDF5 source files." << std::endl;
    }
    sendLog();
    
    int inputHdf5Count = Hdf5ManagerCheetah::cheetahManagerCount();
    
    for (int i = 0; i < inputHdf5Count; i++)
    {
        Hdf5ManagerCheetahPtr manager = Hdf5ManagerCheetah::cheetahManager(i);
        
        for (int j = 0; j < manager->imageAddressCount(); j++)
        {
            if (!loadAll)
            {
                if (count < startImage || count >= endImage)
                {
                    count++;
                    continue;
                }
            }
            
            std::string imageAddress = manager->imageAddress(j);
            
            std::string imgName = manager->lastComponent(imageAddress) + ".img";
            
            if (!hasList)
            {
                Hdf5ImagePtr hdf5Image = Hdf5ImagePtr(new Hdf5Image(imgName, wavelength,
                                                                    detectorDistance));
                ImagePtr newImage = boost::static_pointer_cast<Image>(hdf5Image);
                hdf5Image->loadCrystals();
                newImages->push_back(newImage);
            }

            count++;
        }
    }
    
    for (int i = 0; i < images.size(); i++)
    {
        for (int j = 0; j < images[i]->mtzCount(); j++)
        {
            MtzPtr mtz = images[i]->mtz(j);
            
            mtzManagers.push_back(mtz);
        }
    }
    
    logged << "N: Images loaded from HDF5: " << newImages->size() << std::endl;;
    
    sendLog();
}

void MtzRefiner::readMatricesAndMtzs()
{
    std::string filename = FileParser::getKey("ORIENTATION_MATRIX_LIST",
                                              std::string(""));
    
    double version = FileParser::getKey("MATRIX_LIST_VERSION", 2.0);
    
    if (version >= 2.0)
    {
        readMatricesAndImages(NULL, false);
        
        return;
    }
    
    if (filename == "")
    {
        std::cout
        << "Orientation matrix list has not been provided for refinement. Please provide under keyword ORIENTATION_MATRIX_LIST."
        << std::endl;
        exit(1);
    }
    
    std::ostringstream logged;
    
    if (mtzManagers.size() > 0)
    {
        logged << "Mtzs already present; not reloading from list" << std::endl;
        Logger::mainLogger->addStream(&logged);
        return;
    }
    
    logged << "Loading MTZs from list" << std::endl;
    
    const std::string contents = FileReader::get_file_contents(
                                                               filename.c_str());
    
    vector<std::string> lines = FileReader::split(contents, '\n');
    
    int maxThreads = FileParser::getMaxThreads();
    
    boost::thread_group threads;
    
    vector<vector<MtzPtr> > managerSubsets;
    managerSubsets.resize(maxThreads);
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(singleThreadRead, lines,
                                               &managerSubsets[i], i);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    int total = 0;
    
    for (int i = 0; i < maxThreads; i++)
    {
        total += managerSubsets[i].size();
    }
    
    mtzManagers.reserve(total);
    int lastPos = 0;
    
    for (int i = 0; i < maxThreads; i++)
    {
        mtzManagers.insert(mtzManagers.begin() + lastPos,
                           managerSubsets[i].begin(), managerSubsets[i].end());
        lastPos += managerSubsets[i].size();
    }
    
    logged << "Mtz count: " << mtzManagers.size() << std::endl;
    Logger::mainLogger->addStream(&logged);
}

void MtzRefiner::singleThreadRead(vector<std::string> lines,
                                  vector<MtzPtr> *mtzManagers, int offset)
{
    int maxThreads = FileParser::getMaxThreads();
    
    int end = imageMax(lines.size());
    
    int skip = FileParser::getKey("IMAGE_SKIP", 0);
    vector<double> unitCell = FileParser::getKey("UNIT_CELL", vector<double>());
    
    bool checkingUnitCell = false;
    
    if (skip > lines.size())
    {
        std::cout << "Mtz skip beyond Mtz count" << std::endl;
        exit(1);
    }
    
    if (skip > 0)
    {
        std::cout << "Skipping " << skip << " lines" << std::endl;
    }
    
    for (int i = skip + offset; i < end; i += maxThreads)
    {
        std::ostringstream log;
        
        vector<std::string> components = FileReader::split(lines[i], ' ');
        
        if (components.size() < 10)
            continue;
        
        std::string mtzName = components[0] + ".mtz";
        if (!FileReader::exists(mtzName))
        {
            log << "Skipping file " << mtzName << std::endl;
            continue;
        }
        
        double matrix[9];
        
        readMatrix(matrix, lines[i]);
        
        MtzPtr newManager = MtzPtr(new MtzManager());
        
        newManager->setFilename(mtzName.c_str());
        newManager->setMatrix(matrix);
        newManager->loadReflections(1);
        newManager->setSigmaToUnity();
        
        if (newManager->reflectionCount() > 0)
        {
            mtzManagers->push_back(newManager);
        }
        
        Logger::mainLogger->addStream(&log);
    }
}

void MtzRefiner::readMatrix(double (&matrix)[9], std::string line)
{
    vector<std::string> components = FileReader::split(line, ' ');
    
    for (int j = 1; j <= 9; j++)
    {
        std::string component = components[j];
        
        /* Locate the substring to replace. */
        int index = (int)component.find(std::string("*^"), 0);
        if (index != std::string::npos)
        {
            component.replace(index, 2, std::string("e"));
        }
        
        double matVar = atof(component.c_str());
        matrix[j - 1] = matVar;
    }
}

// MARK: Merging

void MtzRefiner::correlationAndInverse(bool shouldFlip)
{
    if (MtzManager::getReferenceManager() == NULL)
    {
        MtzManager::setReference(&*reference);
    }
    
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        double correl = mtzManagers[i]->correlation(true);
        double invCorrel = correl;
        
        if (MtzManager::getReferenceManager()->ambiguityCount() > 1)
        {
            mtzManagers[i]->setActiveAmbiguity(1);
            invCorrel = mtzManagers[i]->correlation(true);
            mtzManagers[i]->setActiveAmbiguity(0);
            
            if (invCorrel > correl && shouldFlip)
                mtzManagers[i]->setActiveAmbiguity(1);
        }
        double newCorrel = mtzManagers[i]->correlation(true);
        mtzManagers[i]->setRefCorrelation(newCorrel);
        
        std::cout << mtzManagers[i]->getFilename() << "\t" << correl << "\t"
        << invCorrel << std::endl;
    }
}

void MtzRefiner::linearScaling()
{
    loadInitialMtz();
    readMatricesAndMtzs();
    
    MtzMerger merger;
    merger.setSomeMtzs(mtzManagers);
    merger.setCycle(-1);
    merger.setScalingType(ScalingTypeReference);
    merger.setFilename("scaling.mtz");
    merger.scale();
    
    referencePtr = merger.getMergedMtz();
}

void MtzRefiner::merge(bool mergeOnly)
{
    setupFreeMillers();
    
    loadInitialMtz();
    MtzManager::setReference(&*reference);
    readRefinedMtzs = true;
    readMatricesAndMtzs();
    
    bool partialityRejection = FileParser::getKey("PARTIALITY_REJECTION", false);
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        /*    mtzManagers[i]->excludeFromLogCorrelation();
         if (partialityRejection)
         mtzManagers[i]->excludePartialityOutliers(); */
    }
    
    correlationAndInverse(true);
    
    double correlationThreshold = FileParser::getKey("CORRELATION_THRESHOLD",
                                                     CORRELATION_THRESHOLD);
    
    vector<MtzPtr> idxOnly;
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        if ((mtzManagers[i]->getActiveAmbiguity() == 0))
            idxOnly.push_back(mtzManagers[i]);
    }
    
    bool mergeAnomalous = FileParser::getKey("MERGE_ANOMALOUS", false);
    
    int scalingInt = FileParser::getKey("SCALING_STRATEGY",
                                        (int) SCALING_STRATEGY);
    ScalingType scaling = (ScalingType) scalingInt;
    bool old = FileParser::getKey("OLD_MERGE", true);
    
    if (!old)
    {
        MtzMerger merger;
        merger.setAllMtzs(mtzManagers);
        merger.setCycle(-1);
        merger.setScalingType(scaling);
        merger.setFilename("remerged.mtz");
        merger.mergeFull();
        
        if (mergeAnomalous)
        {
            merger.mergeFull(true);
        }
        
        merger.setFreeOnly(true);
        merger.mergeFull();
        
        referencePtr = merger.getMergedMtz();
    }
    else
    {
        MtzGrouper *grouper = new MtzGrouper();
        grouper->setScalingType(scaling);
        grouper->setWeighting(WeightTypePartialitySigma);
        grouper->setMtzManagers(mtzManagers);
        
        grouper->setCutResolution(false);
        
        grouper->setCorrelationThreshold(correlationThreshold);
        
        MtzPtr mergedMtz, unmergedMtz;
        grouper->merge(&mergedMtz, &unmergedMtz, -1, mergeAnomalous);
        mergedMtz->writeToFile("remerged.mtz");
        
        delete grouper;
    }
}

// MARK: Integrating images

void MtzRefiner::integrateImagesWrapper(MtzRefiner *object,
                                        vector<MtzPtr> *&mtzSubset, int offset, bool orientation)
{
    object->integrateImages(mtzSubset, offset, orientation);
}

void MtzRefiner::integrateImages(vector<MtzPtr> *&mtzSubset,
                                 int offset, bool orientation)
{
    int maxThreads = FileParser::getMaxThreads();
    
    for (int i = offset; i < images.size(); i += maxThreads)
    {
        std::ostringstream logged;
        logged << "Integrating image " << i << std::endl;
        Logger::mainLogger->addStream(&logged);
        
        images[i]->clearMtzs();
        
        if (orientation)
            images[i]->refineOrientations();
        
        vector<MtzPtr> mtzs = images[i]->currentMtzs();
        
        for (int j = 0; j < mtzs.size(); j++)
        {
            images[i]->addMtz(mtzs[j]);
        }
        
        mtzSubset->insert(mtzSubset->end(), mtzs.begin(), mtzs.end());
        images[i]->dropImage();
    }
}

void MtzRefiner::maximumImageThread(MtzRefiner *me, ImagePtr maxImage, int offset)
{
    std::vector<ImagePtr> selection;
    int maxThreads = FileParser::getMaxThreads();
    
    for (int i = offset; i < me->images.size(); i += maxThreads)
    {
        selection.push_back(me->images[i]);
    }
    
    maxImage->makeMaximumFromImages(selection);
}

void MtzRefiner::maximumImage()
{
    if (images.size() == 0)
    {
        loadImageFiles();
    }
    
    ImagePtr maximum = ImagePtr(new Image("maxImage.img", 0, 0));
    int maxThreads = FileParser::getMaxThreads();
    
    std::vector<ImagePtr> maxSelections;
    
    for (int i = 0; i < maxThreads; i++)
    {
        ImagePtr anImage = ImagePtr(new Image("maxImage_" + i_to_str(i) + ".img", 0, 0));
        maxSelections.push_back(anImage);
    }

    boost::thread_group threads;
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(maximumImageThread, this, maxSelections[i], i);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    maximum->makeMaximumFromImages(maxSelections, true);
    Detector::setDrawImage(maximum);
}

void MtzRefiner::loadImageFiles()
{
    loadPanels();
    
    std::string filename = FileParser::getKey("ORIENTATION_MATRIX_LIST",
                                              std::string(""));
    std::string hdf5 = FileParser::getKey("HDF5_OUTPUT_FILE",
                                          std::string(""));
    
    if (filename == "" && hdf5 == "")
    {
        logged
        << "Filename list has not been provided for integration. Please provide under keyword ORIENTATION_MATRIX_LIST."
        << std::endl;
        sendLog(LogLevelNormal);
    }
    
    readMatricesAndImages(&filename);
    
    if (images.size() > 0)
    {
        Detector::setDrawImage(images[0]);
        // force loading of everything
        images[0]->valueAt(0, 0);
        SpotPtr spot = SpotPtr(new Spot(images[0]));
    }
}

void MtzRefiner::integrate()
{
    bool orientation = FileParser::getKey("REFINE_ORIENTATIONS", true); // unused I think
    
    loadImageFiles();
    
    int crystals = 0;
    
    for (int i = 0; i < images.size(); i++)
    {
        crystals += images[i]->IOMRefinerCount();
    }
    
    logged << images.size() << " images with " << crystals << " crystal orientations." << std::endl;
    sendLog();
    
    mtzManagers.clear();
    
    for (int i = 0; i < images.size(); i++)
    {
        images[i]->clearMtzs();
    }
    
    int maxThreads = FileParser::getMaxThreads();
    
    boost::thread_group threads;
    vector<vector<MtzPtr> > managerSubsets;
    managerSubsets.resize(maxThreads);
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(integrateImagesWrapper, this,
                                               &managerSubsets[i], i, orientation);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    int total = 0;
    
    for (int i = 0; i < maxThreads; i++)
    {
        total += managerSubsets[i].size();
    }
    
    logged << "N: Total images loaded: " << total << std::endl;
    sendLog();
    
    mtzManagers.reserve(total);
    int lastPos = 0;
    
    for (int i = 0; i < maxThreads; i++)
    {
        mtzManagers.insert(mtzManagers.begin() + lastPos,
                           managerSubsets[i].begin(), managerSubsets[i].end());
        lastPos += managerSubsets[i].size();
    }
    
    writeNewOrientations(false, true);
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        mtzManagers[i]->writeToDat();
    }
    
    integrationSummary();
}


void MtzRefiner::integrationSummary()
{
    std::ostringstream refineSummary;
    
    int imageCount = images.size();
    int iomRefinerCount = 0;
    
    refineSummary << IOMRefiner::refinementSummaryHeader() << std::endl;
    
    for (int i = 0; i < images.size(); i++)
    {
        for (int j = 0; j < images[i]->IOMRefinerCount(); j++)
        {
            refineSummary << images[i]->getIOMRefiner(j)->refinementSummary() << std::endl;
            
            iomRefinerCount++;
        }
    }
    
    std::string summaryString = refineSummary.str();
    Logger::mainLogger->addStream(&refineSummary);
    
    std::replace(summaryString.begin(), summaryString.end(), '\t', ',');
    
    std::ofstream summaryCSV;
    summaryCSV.open("integration.csv");
    summaryCSV << summaryString;
    summaryCSV.close();
    
    float percentage = (float)iomRefinerCount / (float)imageCount * 100;
    logged << "Indexed " << iomRefinerCount << " images out of " << imageCount << " (" << percentage << "%)." << std::endl;
    sendLog();
    
    Logger::mainLogger->addString("Written integration summary to integration.csv");
    
}

void MtzRefiner::loadPanels(bool mustFail)
{
    bool useNewDetectorFormat = Detector::isActive();
    
    if (!useNewDetectorFormat && mustFail)
    {
        logged << "DETECTOR_LIST has not been provided." << std::endl;
        FileParser::printCommandInfo("DETECTOR_LIST");
        sendLogAndExit();
        return;
    }
    
    if (useNewDetectorFormat)
    {
        if (Detector::getMaster())
        {
            return;
        }
        
        std::string detectorList = FileParser::getKey("DETECTOR_LIST", std::string(""));
        
        logged << "Loading new detector format from " << detectorList << "." << std::endl;
        sendLog();
        int formatInt = FileParser::getKey("GEOMETRY_FORMAT", 0);
        
        GeometryFormat format = (GeometryFormat)formatInt;
        
        GeometryParser geomParser = GeometryParser(detectorList, format);
        geomParser.parse();
        
        return;
    }
}

// MARK: indexing

void MtzRefiner::writeAllNewOrientations()
{
    std::string filename = FileParser::getKey("NEW_MATRIX_LIST", std::string("new_orientations.dat"));
    bool includeRots = false;
    
    std::ofstream allMats;
    allMats.open(FileReader::addOutputDirectory("all-" + filename));
    
    allMats << "version 3.0" << std::endl;
    
    for (int i = 0; i < images.size(); i++)
    {
        std::string imageName = images[i]->getBasename();
        allMats << "image " << imageName << std::endl;
        
        if (images[i]->getSpotsFile().length() > 0)
        {
            allMats << "spots " << images[i]->getSpotsFile() << std::endl;
        }
        
        for (int j = 0; j < images[i]->mtzCount(); j++)
        {
            MtzPtr mtz = images[i]->mtz(j);
            int crystalNum = j;
            
            MatrixPtr matrix = mtz->getMatrix()->copy();
            
            if (includeRots)
            {
                double hRad = mtz->getHRot() * M_PI / 180;
                double kRad = mtz->getKRot() * M_PI / 180;
                
                matrix->rotate(hRad, kRad, 0);
            }
            
            allMats << "crystal " << crystalNum << std::endl;
            std::string description = matrix->description(true);
            allMats << description << std::endl;
        }
    }
    
    Logger::mainLogger->addString("Written to matrix list: all-" + filename);
    allMats.close();
}

void MtzRefiner::writeNewOrientations(bool includeRots, bool detailed)
{
    std::ofstream mergeMats;
    std::ofstream refineMats;
    std::ofstream integrateMats;
    
    std::string filename = FileParser::getKey("NEW_MATRIX_LIST", std::string("new_orientations.dat"));
    
    mergeMats.open(FileReader::addOutputDirectory("merge-" + filename));
    refineMats.open(FileReader::addOutputDirectory("refine-" + filename));
    integrateMats.open(FileReader::addOutputDirectory("integrate-" + filename));
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        MtzPtr manager = mtzManagers[i];
        
        // write out matrices etc.
        std::string imgFilename = manager->filenameRoot();
        
        ImagePtr image = manager->getImagePtr();
        std::string parentName;
        
        if (image)
        {
            parentName = image->getBasename();
        }
        
        MatrixPtr matrix = manager->getMatrix()->copy();
        
        if (includeRots)
        {
            double hRad = manager->getHRot() * M_PI / 180;
            double kRad = manager->getKRot() * M_PI / 180;
            
            matrix->rotate(hRad, kRad, 0);
        }
        
        std::string prefix = "img-";
        
        if (imgFilename.substr(0, 3) == "img")
            prefix = "";
        
        
        if (!detailed)
        {
            refineMats << prefix << imgFilename << " ";
            std::string description = matrix->description();
            refineMats << description << std::endl;
        }
        else
        {
            refineMats << "image " << prefix << imgFilename << std::endl;
            mergeMats << "image ref-" << prefix << imgFilename << std::endl;
            refineMats << "parent " << imgFilename << std::endl;
            mergeMats << "parent " << imgFilename << std::endl;
            std::string description = matrix->description(true);
            refineMats << description << std::endl;
            mergeMats << description << std::endl;
            
            if (hasRefined)
            {
                refineMats << manager->getParamLine() << std::endl;
            }
        }
    }
    
    for (int i = 0; i < images.size(); i++)
    {
        ImagePtr image = images[i];
        
        // write out matrices etc.
        std::string imgFilename = image->filenameRoot();
        
        integrateMats << "image " << imgFilename << std::endl;
        
        for (int j = 0; j < image->IOMRefinerCount(); j++)
        {
            MatrixPtr matrix = image->getIOMRefiner(j)->getMatrix()->copy();
            
            matrix->rotate(0, 0, -M_PI / 2);
            std::string desc90 = matrix->description(true);
            integrateMats << desc90 << std::endl;
        }
        
        if (image->getSpotsFile().length() > 0)
        {
            integrateMats << "spots " << image->getSpotsFile() << std::endl;
        }
        
        integrateMats << "wavelength " << image->getWavelength() << std::endl;
    }
    
    Logger::mainLogger->addString("Written to filename set: " + filename);
    
    refineMats.close();
    mergeMats.close();
    integrateMats.close();
    
    writeAllNewOrientations();
}

void MtzRefiner::findSpotsThread(MtzRefiner *me, int offset)
{
    double maxThreads = FileParser::getMaxThreads();
    
    for (int i = offset; i < me->images.size(); i += maxThreads)
    {
        me->images[i]->processSpotList();
        me->images[i]->dropImage();
    }
}

void MtzRefiner::index()
{
    loadImageFiles();
    
    logged << "N: Total images loaded: " << images.size() << std::endl;
    sendLog();
    
    if (!indexManager)
        indexManager = new IndexManager(images);
    
    indexManager->index();
    
    mtzManagers = indexManager->getMtzs();
    
    writeNewOrientations(false, true);
    integrationSummary();
}


void MtzRefiner::combineLists()
{
    loadPanels();
    this->readMatricesAndImages();
    std::cout << "N: Total images loaded in file 1: " << images.size() << std::endl;
    
    std::string secondList = FileParser::getKey("SECOND_MATRIX_LIST", std::string(""));
    std::vector<ImagePtr> secondImages;
    
    this->readMatricesAndImages(&secondList, true, &secondImages);
    std::cout << "N: Total images loaded in file 2 (" << secondList << "): " << secondImages.size() << std::endl;
    
    if (!indexManager)
        indexManager = new IndexManager(images);
    
    indexManager->setMergeImages(secondImages);
    indexManager->combineLists();
    
    writeNewOrientations();
    integrationSummary();
}


void MtzRefiner::powderPattern()
{
    loadImageFiles();
    
    if (!indexManager)
        indexManager = new IndexManager(images);
    
    double maxThreads = FileParser::getMaxThreads();
    boost::thread_group threads;
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(findSpotsThread, this, i);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    indexManager->powderPattern();
}

void MtzRefiner::indexingParameterAnalysis()
{
    if (!indexManager)
        indexManager = new IndexManager(images);
    
    indexManager->indexingParameterAnalysis();
}

// MARK: SACLA stuff for April 2016


void MtzRefiner::integrateSpotsThread(MtzRefiner *me, int offset)
{
    double maxThreads = FileParser::getMaxThreads();
    
    for (int i = offset; i < me->images.size(); i += maxThreads)
    {
        me->images[i]->integrateSpots();
    }
}

void MtzRefiner::integrateSpots()
{
    loadPanels();
    this->readMatricesAndImages();
    std::cout << "N: Total images loaded: " << images.size() << std::endl;
    double maxThreads = FileParser::getMaxThreads();
    
    boost::thread_group threads;
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(integrateSpotsThread, this, i);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    std::cout << "Finished integrating spots." << std::endl;
}

// MARK: Miscellaneous

void MtzRefiner::plotIntegrationWindows()
{
    int h, k, l;
    std::vector<int> hkl = FileParser::getKey("MILLER_INDEX", std::vector<int>());
    GraphDrawer drawer = GraphDrawer(NULL);
    
    if (hkl.size() < 3)
    {
        logged << "Please set MILLER_INDEX" << std::endl;
        sendLog();
    }
    else
    {
        h = hkl[0];
        k = hkl[1];
        l = hkl[2];
        
        logged << "Drawing integration windows for " << h << " " << k << " " << l << " for " << mtzManagers.size() << " mtzs." << std::endl;
        sendLog();
        
        drawer.cutoutIntegrationAreas(mtzManagers, h, k, l);
    }

}

void MtzRefiner::plotIntensities()
{
    int h, k, l;
    std::vector<int> hkl = FileParser::getKey("MILLER_INDEX", std::vector<int>());
    GraphDrawer drawer = GraphDrawer(NULL);
    
    if (hkl.size() < 3)
    {
        logged << "To plot a specific Miller index please use MILLER_INDEX; this will dump a huge number" << std::endl;
        sendLog();
        drawer.plotReflectionFromMtzs(getMtzManagers());
    }
    else
    {
        h = hkl[0];
        k = hkl[1];
        l = hkl[2];
        
        drawer.plotReflectionFromMtzs(getMtzManagers(), h, k, l);
    }
    
    
}

void MtzRefiner::refineMetrology(bool global)
{
    loadImageFiles();
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        mtzManagers[i]->millersToDetector();
    }
    
    GeometryRefiner refiner = GeometryRefiner();
    refiner.setImages(images);
    refiner.refineGeometry();
}

void MtzRefiner::reportMetrology()
{
    GeometryRefiner refiner = GeometryRefiner();
    refiner.setImages(images);
    refiner.startingGraphs();
    refiner.reportProgress();
    
    Detector::getMaster()->reportMillerScores();
}

void MtzRefiner::fakeSpotsThread(std::vector<ImagePtr> *images, int offset)
{
    int maxThreads = FileParser::getMaxThreads();
    
    for (int i = offset; i < images->size(); i += maxThreads)
    {
        ImagePtr image = images->at(i);
        
        image->fakeSpots();
    }
}

void MtzRefiner::fakeSpots()
{
    loadImageFiles();
    
    boost::thread_group threads;
    
    int maxThreads = FileParser::getMaxThreads();
    
    for (int i = 0; i < maxThreads; i++)
    {
        boost::thread *thr = new boost::thread(fakeSpotsThread, &images, i);
        threads.add_thread(thr);
    }
    
    threads.join_all();
    
    writeAllNewOrientations();
}

MtzRefiner::~MtzRefiner()
{
    //    std::cout << "Deallocating MtzRefiner." << std::endl;
    
    mtzManagers.clear();
    
    images.clear();
    
    // TODO Auto-generated destructor stub
}

void MtzRefiner::removeSigmaValues()
{
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        mtzManagers[i]->setSigmaToUnity();
    }
    
    if (MtzManager::getReferenceManager() != NULL)
        MtzManager::getReferenceManager()->setSigmaToUnity();
}


void MtzRefiner::displayIndexingHands()
{
    std::ostringstream idxLogged, invLogged;
    
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        std::ostringstream &which = mtzManagers[i]->getActiveAmbiguity() == 0 ? idxLogged : invLogged;
        
        which << mtzManagers[i]->getFilename() << std::endl;
    }
    
    Logger::mainLogger->addString("**** Indexing hands ****\n - First indexing hand");
    Logger::mainLogger->addStream(&idxLogged);
    Logger::mainLogger->addString("- Second indexing hand");
    Logger::mainLogger->addStream(&invLogged);
    
}

void MtzRefiner::applyUnrefinedPartiality()
{
    for (int i = 0; i < mtzManagers.size(); i++)
    {
        mtzManagers[i]->applyUnrefinedPartiality();
    }
}

void MtzRefiner::orientationPlot()
{
    GraphDrawer drawer = GraphDrawer(NULL);
    drawer.plotOrientationStats(mtzManagers);
}

int MtzRefiner::imageSkip(size_t totalCount)
{
    int skip = FileParser::getKey("IMAGE_SKIP", 0);
    
    if (skip > totalCount)
    {
        std::cout << "Image skip beyond image count" << std::endl;
        exit(1);
    }
    
    return skip;
}

void MtzRefiner::writePNGs(int total)
{
    loadPanels();
    
    if (!images.size())
    {
        readMatricesAndImages();
    }
    
    if (total == 0)
    {
        total = FileParser::getKey("PNG_TOTAL", 10);
    }
    
    int totalImages = (int)images.size();
    int skip = totalImages / total;
    
    if (skip == 0)
        skip = 1;
    
    bool allLattices = FileParser::getKey("PNG_ALL_LATTICES", false);
    
    for (int i = 0; i < totalImages; i += skip)
    {
        images[i]->drawSpotsOnPNG();
        
        if (allLattices && images[i]->mtzCount() > 0)
        {
            images[i]->drawCrystalsOnPNG(-1);
        }
        else
        {
            for (int j = 0; j < images[i]->mtzCount(); j++)
            {
                images[i]->drawCrystalsOnPNG(j);
            }
        }
    }
}

void MtzRefiner::takeTwoPNG()
{
    ImagePtr image = Detector::getSpecialImage();
    
    if (!image)
    {
        logged << "No images/special image loaded, cannot do TakeTwo vector plot." << std::endl;
        sendLog();
        return;
    }
    
    image->plotTakeTwoVectors(images);
}

void MtzRefiner::imageToDetectorMap()
{
    if (images.size())
    {
        double min, max;
        Detector::getMaster()->zLimits(&min, &max);
    }
}