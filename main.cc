#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

#include <H5Cpp.h>
#include <fitsio.h>

using namespace H5;
using namespace std;

#define SCHEMA_VERSION "0.1"
#define HDF5_CONVERTER "hdf_convert"
#define HDF5_CONVERTER_VERSION "0.1.4"

// Stolen from From https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
// trim from start (in place)
static inline void ltrim(string& s) {
    s.erase(s.begin(), find_if(s.begin(), s.end(), [](int ch) { return !isspace(ch); }));
}

// trim from end (in place)
static inline void rtrim(string& s) {
    s.erase(find_if(s.rbegin(), s.rend(), [](int ch) { return !isspace(ch); }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(string& s) {
    ltrim(s);
    rtrim(s);
}

// End stolen from https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        cout << "Usage: hdf_convert {INPUT FITS file} {OUTPUT HDF5 file}" << endl;
        return 1;
    }

    string inputFileName = argv[1];
    string outputFileName;
    if (argc == 3) {
        outputFileName = argv[2];
    } else {
        auto fitsIndex = inputFileName.find_last_of(".fits");
        if (fitsIndex != string::npos) {
            outputFileName = inputFileName.substr(0, fitsIndex - 4);
            outputFileName += ".hdf5";
        } else {
            outputFileName = inputFileName + ".hdf5";
        }
    }
    cout << "Converting FITS file " << inputFileName << " to HDF5 file " << outputFileName << endl;

    FloatType floatDataType(PredType::NATIVE_FLOAT);
    floatDataType.setOrder(H5T_ORDER_LE);
    IntType int64Type(PredType::NATIVE_INT64);
    int64Type.setOrder(H5T_ORDER_LE);

    auto tStart = chrono::high_resolution_clock::now();

    fitsfile* inputFilePtr;
    int status = 0;
    int bitpix;
    int N;
    fits_open_file(&inputFilePtr, inputFileName.c_str(), READONLY, &status);
    if (status != 0) {
        cout << "error opening FITS file" << endl;
        return 1;
    }
    fits_get_img_type(inputFilePtr, &bitpix, &status);
    fits_get_img_dim(inputFilePtr, &N, &status);

    if (bitpix != -32) {
        cout << "Currently only supports FP32 files" << endl;
        return 1;
    }

    if (N < 2 || N > 4) {
        cout << "Currently only supports 2D, 3D and 4D cubes" << endl;
        return 1;
    }

    long dims[4];
    fits_get_img_size(inputFilePtr, 4, dims, &status);

    auto stokes = N == 4 ? dims[3] : 1;
    auto depth = N >= 3 ? dims[2] : 1;
    auto height = dims[1];
    auto width = dims[0];

    int numBinsHistXY = sqrt(width * height) + 0.5;
    int numBinsHistXYZ = numBinsHistXY;

    vector<hsize_t> standardDims = {height, width};
    vector<hsize_t> swizzledDims = {width, height};
    vector<hsize_t> xyHistogramDims = {numBinsHistXY};
    vector<hsize_t> xyStatsDims = {};
    vector<hsize_t> zStatsDims = {height, width};
    vector<hsize_t> xyzHistogramDims = {numBinsHistXYZ};
    vector<hsize_t> xyzStatsDims = {};

    if (N >= 3) {
        standardDims.insert(standardDims.begin(), depth);
        swizzledDims.push_back(depth);
        xyHistogramDims.insert(xyHistogramDims.begin(), depth);
        xyStatsDims.insert(xyStatsDims.begin(), depth);
    }
    if (N == 4) {
        standardDims.insert(standardDims.begin(), stokes);
        swizzledDims.insert(swizzledDims.begin(), stokes);
        xyHistogramDims.insert(xyHistogramDims.begin(), stokes);
        xyStatsDims.insert(xyStatsDims.begin(), stokes);
        zStatsDims.insert(zStatsDims.begin(), stokes);
        xyzHistogramDims.insert(xyzHistogramDims.begin(), stokes);
        xyzStatsDims.insert(xyzStatsDims.begin(), stokes);
    }

    DataSpace swizzledDataSpace(N, swizzledDims.data());
    DataSpace standardDataSpace(N, standardDims.data());

    string tempOutputFileName = outputFileName + ".tmp";
    H5File outputFile(tempOutputFileName, H5F_ACC_TRUNC);
    auto outputGroup = outputFile.createGroup("0");

    // Headers
    DataSpace attributeDataSpace = DataSpace(H5S_SCALAR);
    StrType strType(PredType::C_S1, 256);
    Attribute attribute = outputGroup.createAttribute("SCHEMA_VERSION", strType, attributeDataSpace);
    attribute.write(strType, SCHEMA_VERSION);
    attribute = outputGroup.createAttribute("HDF5_CONVERTER", strType, attributeDataSpace);
    attribute.write(strType, HDF5_CONVERTER);
    attribute = outputGroup.createAttribute("HDF5_CONVERTER_VERSION", strType, attributeDataSpace);
    attribute.write(strType, HDF5_CONVERTER_VERSION);

    int numHeaders;
    fits_get_hdrspace(inputFilePtr, &numHeaders, NULL, &status);
    char headerTmp[255];
    for (auto i = 0; i < numHeaders; i++) {
        fits_read_record(inputFilePtr, i, headerTmp, &status);
        string headerLine(headerTmp);
        if (headerLine.find("COMMENT") == 0) {
        } else if (headerLine.find("HISTORY") == 0) {
        } else {
            auto eqPos = headerLine.find('=');
            auto commentPos = headerLine.find_last_of('/');
            if (eqPos != string::npos) {
                auto attributeName = headerLine.substr(0, eqPos);
                auto endPos = commentPos != string::npos ? commentPos - eqPos - 1 : string::npos;
                string attributeValue = headerLine.substr(eqPos + 1, endPos);
                trim(attributeName);
                trim(attributeValue);
                if (attributeValue.length() >= 2 && attributeValue.find('\'') == 0 &&
                    attributeValue.find_last_of('\'') == attributeValue.length() - 1) {
                    attributeValue = attributeValue.substr(1, attributeValue.length() - 2);
                    trim(attributeValue);
                }
                if (!outputGroup.attrExists(attributeName)) {
                    attribute = outputGroup.createAttribute(attributeName, strType, attributeDataSpace);
                    attribute.write(strType, attributeValue);
                } else {
                    cout << "Warning: Skipping duplicate attribute '" << attributeName << "'" << endl;
                }
            }
        }
    }

    if (depth > 1) {
        auto swizzledGroup = outputGroup.createGroup("SwizzledData");
        string swizzledName = N == 3 ? "ZYX" : "ZYXW";
        auto swizzledDataSet = swizzledGroup.createDataSet(swizzledName, floatDataType, swizzledDataSpace);
    }

    auto standardDataSet = outputGroup.createDataSet("DATA", floatDataType, standardDataSpace);

    auto cubeSize = depth * height * width;
    cout << "Allocating " << cubeSize * 4 * 2 * 1e-9 << " GB of memory..." << flush;
    auto tStartAlloc = chrono::high_resolution_clock::now();

    float* standardCube = new float[cubeSize];
    
    vector<float> minValsXY(depth * stokes);
    vector<float> maxValsXY(depth * stokes);
    vector<float> meanValsXY(depth * stokes);
    vector<int64_t> nanValsXY(depth * stokes);
    
    vector<int64_t> histogramsXY(depth * stokes * numBinsHistXY);
    
    float* rotatedCube;
    
    vector<float> minValsZ;
    vector<float> maxValsZ;
    vector<float> meanValsZ;
    vector<int64_t> nanValsZ;

    vector<float> minValsXYZ;
    vector<float> maxValsXYZ;
    vector<float> meanValsXYZ;
    vector<int64_t> nanValsXYZ;
    
    // XYZ histograms calculated using OpenMP and summed later
    vector<int64_t> partialHistogramsXYZ;
    vector<int64_t> histogramsXYZ;
    
    if (depth > 1) {
        rotatedCube = new float[cubeSize];
        
        minValsZ.resize(width * height * stokes, numeric_limits<float>::max());
        maxValsZ.resize(width * height * stokes, -numeric_limits<float>::max());
        meanValsZ.resize(width * height * stokes);
        nanValsZ.resize(width * height * stokes);

        minValsXYZ.resize(stokes);
        maxValsXYZ.resize(stokes);
        meanValsXYZ.resize(stokes);
        nanValsXYZ.resize(stokes);
        
        // XYZ histograms calculated using OpenMP and summed later
        partialHistogramsXYZ.resize(depth * stokes * numBinsHistXYZ);
        histogramsXYZ.resize(stokes * numBinsHistXYZ);
    }

    auto tEndAlloc = chrono::high_resolution_clock::now();
    auto dtAlloc = chrono::duration_cast<chrono::milliseconds>(tEndAlloc - tStartAlloc).count();
    cout << "Done in " << dtAlloc * 1e-3 << " seconds" << endl;

    for (unsigned int currentStokes = 0; currentStokes < stokes; currentStokes++) {
        // Read data into memory space
        hsize_t memDims[] = {depth, height, width};
        DataSpace memspace(3, memDims);

        cout << "Reading Stokes " << currentStokes << " dataset..." << flush;
        auto tStartRead = chrono::high_resolution_clock::now();
        long fpixel[] = {1, 1, 1, currentStokes + 1};
        fits_read_pix(inputFilePtr, TFLOAT, fpixel, cubeSize, NULL, standardCube, NULL, &status);
        auto tEndRead = chrono::high_resolution_clock::now();
        auto dtRead = chrono::duration_cast<chrono::milliseconds>(tEndRead - tStartRead).count();
        float readSpeed = (cubeSize * 4) * 1.0e-6 / (dtRead * 1.0e-3);
        cout << "Done in " << dtRead * 1e-3 << " seconds (" << readSpeed << " MB/s)" << endl;

        cout << "Processing Stokes " << currentStokes << " dataset..." << flush;
        auto tStartProcess = chrono::high_resolution_clock::now();

        // First loop calculates stats for each XY slice and rotates the dataset
#pragma omp parallel for
        for (auto i = 0; i < depth; i++) {
            float minVal = numeric_limits<float>::max();
            float maxVal = -numeric_limits<float>::max();
            float sum = 0;
            int64_t nanCount = 0;

            for (auto j = 0; j < height; j++) {
                for (auto k = 0; k < width; k++) {
                    auto sourceIndex = k + width * j + (height * width) * i;
                    auto destIndex = i + depth * j + (height * depth) * k;
                    auto val = standardCube[sourceIndex];
                    
                    if (depth > 1) {
                        rotatedCube[destIndex] = val;
                    }
                    
                    // Stats
                    if (!isnan(val)) {
                        minVal = min(minVal, val);
                        maxVal = max(maxVal, val);
                        sum += val;
                    } else {
                        nanCount += 1;
                    }
                }
            }

            if (nanCount != (height * width)) {
                minValsXY[currentStokes * depth + i] = minVal;
                maxValsXY[currentStokes * depth + i] = maxVal;
                nanValsXY[currentStokes * depth + i] = nanCount;
                meanValsXY[currentStokes * depth + i] = sum / (height * width - nanCount);
            } else {
                minValsXY[currentStokes * depth + i] = NAN;
                maxValsXY[currentStokes * depth + i] = NAN;
                nanValsXY[currentStokes * depth + i] = nanCount;
                meanValsXY[currentStokes * depth + i] = NAN;
            }
        }
        
        double xyzMin;
        double xyzMax;

        if (depth > 1) {
            // Consolidate XY stats into XYZ stats
            double xyzSum = 0;
            int64_t xyzNanCount = 0;
            xyzMin = minValsXY[currentStokes * depth];
            xyzMax = maxValsXY[currentStokes * depth];

            for (auto i = 0; i < depth; i++) {
                auto meanVal = meanValsXY[currentStokes * depth + i];
                auto nanCount = nanValsXY[currentStokes * depth + i];
                if (!isnan(meanVal)) {
                    xyzSum += meanVal * (height * width - nanCount);
                    xyzMin = fmin(xyzMin, minValsXY[currentStokes * depth + i]);
                    xyzMax = fmax(xyzMax, maxValsXY[currentStokes * depth + i]);
                }
                xyzNanCount += nanCount;
            }

            minValsXYZ[currentStokes] = xyzMin;
            maxValsXYZ[currentStokes] = xyzMax;
            nanValsXYZ[currentStokes] = xyzNanCount;
            if (xyzNanCount != cubeSize) {
                meanValsXYZ[currentStokes] = xyzSum / (cubeSize - xyzNanCount);
            }
        }

        cout << "1..." << flush;

        if (depth > 1) {
            // Second loop calculates stats for each Z profile (i.e. average/min/max XY slices)
#pragma omp parallel for
            for (auto j = 0; j < height; j++) {
                for (auto k = 0; k < width; k++) {
                    float minVal = numeric_limits<float>::max();
                    float maxVal = -numeric_limits<float>::max();
                    float sum = 0;
                    int64_t nanCount = 0;
                    for (auto i = 0; i < depth; i++) {
                        auto sourceIndex = k + width * j + (height * width) * i;
                        auto val = standardCube[sourceIndex];

                        if (!isnan(val)) {
                            minVal = min(minVal, val);
                            maxVal = max(maxVal, val);
                            sum += val;

                        } else {
                            nanCount += 1;
                        }
                    }
                    if (nanCount != (height * width)) {
                        minValsZ[currentStokes * width * height + k + j * width] = minVal;
                        maxValsZ[currentStokes * width * height + k + j * width] = maxVal;
                        nanValsZ[currentStokes * width * height + k + j * width] = nanCount;
                        meanValsZ[currentStokes * width * height + k + j * width] = sum / (depth - nanCount);
                    } else {
                        minValsZ[currentStokes * width * height + k + j * width] = NAN;
                        maxValsZ[currentStokes * width * height + k + j * width] = NAN;
                        nanValsZ[currentStokes * width * height + k + j * width] = nanCount;
                        meanValsZ[currentStokes * width * height + k + j * width] = NAN;
                    }
                }
            }
        }
        
        cout << "2..." << flush;

        // Third loop handles histograms
        
        double cubeMin;
        double cubeMax;
        double cubeRange;
        
        if (depth > 1) {
            cubeMin = xyzMin;
            cubeMax = xyzMax;
            cubeRange = cubeMax - cubeMin;
        }

#pragma omp parallel for
        for (auto i = 0; i < depth; i++) {
            double sliceMin = minValsXY[currentStokes * depth + i];
            double sliceMax = maxValsXY[currentStokes * depth + i];
            double range = sliceMax - sliceMin;

            if (isnan(sliceMin) || isnan(sliceMax) || range == 0) {
                continue;
            }

            for (auto j = 0; j < width * height; j++) {
                auto val = standardCube[i * width * height + j];

                if (!isnan(val)) {
                    // XY Histogram
                    int binIndex = min(numBinsHistXY - 1, (int)(numBinsHistXY * (val - sliceMin) / range));
                    histogramsXY[currentStokes * depth * numBinsHistXY + i * numBinsHistXY + binIndex]++;
                    
                    if (depth > 1) {
                        // XYZ Partial histogram
                        int binIndexXYZ = min(numBinsHistXYZ - 1, (int)(numBinsHistXYZ * (val - cubeMin) / cubeRange));
                        partialHistogramsXYZ[currentStokes * depth * numBinsHistXYZ + i * numBinsHistXYZ + binIndexXYZ]++;
                    }
                }
            }
        }
        
        if (depth > 1) {
            // Consolidate partial XYZ histograms into final histogram
            for (auto i = 0; i < depth; i++) {
                for (auto j = 0; j < numBinsHistXYZ; j++) {
                    histogramsXYZ[currentStokes * numBinsHistXYZ + j] +=
                        partialHistogramsXYZ[currentStokes * depth * numBinsHistXYZ + i * numBinsHistXYZ + j];
                }
            }
        }

        auto tEndRotate = chrono::high_resolution_clock::now();
        auto dtRotate = chrono::duration_cast<chrono::milliseconds>(tEndRotate - tStartProcess).count();
        float rotateSpeed = (cubeSize * 4) * 1.0e-6 / (dtRotate * 1.0e-3);
        cout << "Done in " << dtRotate * 1e-3 << " seconds" << endl;

        cout << "Writing Stokes " << currentStokes << " dataset..." << flush;
        auto tStartWrite = chrono::high_resolution_clock::now();

        auto sliceDataSpace = standardDataSet.getSpace();
        vector<hsize_t> count = {height, width};
        vector<hsize_t> start = {0, 0};

        if (N >= 3) {
            count.insert(count.begin(), depth);
            start.insert(start.begin(), 0);
        }
        if (N == 4) {
            count.insert(count.begin(), 1);
            start.insert(start.begin(), currentStokes);
        }

        sliceDataSpace.selectHyperslab(H5S_SELECT_SET, count.data(), start.data());

        standardDataSet.write(standardCube, PredType::NATIVE_FLOAT, memspace, sliceDataSpace);
        if (depth > 1) {
            auto swizzledGroup = outputGroup.openGroup("SwizzledData");
            string swizzledName = N == 3 ? "ZYX" : "ZYXW";
            auto swizzledDataSet = swizzledGroup.openDataSet(swizzledName);
            swizzledDataSet.write(rotatedCube, PredType::NATIVE_FLOAT, memspace, sliceDataSpace);
        }

        auto tEndWrite = chrono::high_resolution_clock::now();
        auto dtWrite = chrono::duration_cast<chrono::milliseconds>(tEndWrite - tStartWrite).count();
        float writeSpeed = (2 * cubeSize * 4) * 1.0e-6 / (dtWrite * 1.0e-3);
        cout << "Done in " << dtWrite * 1e-3 << " seconds (" << writeSpeed << " MB/s)" << endl;
    }

    auto statsGroup = outputGroup.createGroup("Statistics");

    auto statsXYGroup = statsGroup.createGroup("XY");
    DataSpace xyStatsDataSpace(N - 2, xyStatsDims.data());
    DataSpace xyHistogramDataSpace(N - 1, xyHistogramDims.data());
    auto xyMinDataSet = statsXYGroup.createDataSet("MIN", floatDataType, xyStatsDataSpace);
    auto xyMaxDataSet = statsXYGroup.createDataSet("MAX", floatDataType, xyStatsDataSpace);
    auto xyMeanDataSet = statsXYGroup.createDataSet("MEAN", floatDataType, xyStatsDataSpace);
    auto xyNanCountDataSet = statsXYGroup.createDataSet("NAN_COUNT", int64Type, xyStatsDataSpace);
    auto xyHistogramDataSet = statsXYGroup.createDataSet("HISTOGRAM", int64Type, xyHistogramDataSpace);

    xyMinDataSet.write(minValsXY.data(), PredType::NATIVE_FLOAT);
    xyMaxDataSet.write(maxValsXY.data(), PredType::NATIVE_FLOAT);
    xyMeanDataSet.write(meanValsXY.data(), PredType::NATIVE_FLOAT);
    xyNanCountDataSet.write(nanValsXY.data(), PredType::NATIVE_INT64);
    xyHistogramDataSet.write(histogramsXY.data(), PredType::NATIVE_INT64);

    if (depth > 1) {
        auto statsXYZGroup = statsGroup.createGroup("XYZ");
        DataSpace xyzStatsDataSpace(N - 3, xyStatsDims.data());
        DataSpace xyzHistogramDataSpace(N - 2, xyzHistogramDims.data());
        auto xyzMinDataSet = statsXYZGroup.createDataSet("MIN", floatDataType, xyzStatsDataSpace);
        auto xyzMaxDataSet = statsXYZGroup.createDataSet("MAX", floatDataType, xyzStatsDataSpace);
        auto xyzMeanDataSet = statsXYZGroup.createDataSet("MEAN", floatDataType, xyzStatsDataSpace);
        auto xyzNanCountDataSet = statsXYZGroup.createDataSet("NAN_COUNT", int64Type, xyzStatsDataSpace);
        auto xyzHistogramDataSet = statsXYZGroup.createDataSet("HISTOGRAM", int64Type, xyzHistogramDataSpace);
        xyzMinDataSet.write(minValsXYZ.data(), PredType::NATIVE_FLOAT);
        xyzMaxDataSet.write(maxValsXYZ.data(), PredType::NATIVE_FLOAT);
        xyzMeanDataSet.write(meanValsXYZ.data(), PredType::NATIVE_FLOAT);
        xyzNanCountDataSet.write(nanValsXYZ.data(), PredType::NATIVE_INT64);
        xyzHistogramDataSet.write(histogramsXYZ.data(), PredType::NATIVE_INT64);

        auto statsZGroup = statsGroup.createGroup("Z");
        DataSpace zStatsDataSpace(N - 1, zStatsDims.data());
        auto zMinDataSet = statsZGroup.createDataSet("MIN", floatDataType, zStatsDataSpace);
        auto zMaxDataSet = statsZGroup.createDataSet("MAX", floatDataType, zStatsDataSpace);
        auto zMeanDataSet = statsZGroup.createDataSet("MEAN", floatDataType, zStatsDataSpace);
        auto zNanCountDataSet = statsZGroup.createDataSet("NAN_COUNT", int64Type, zStatsDataSpace);

        zMinDataSet.write(minValsZ.data(), PredType::NATIVE_FLOAT);
        zMaxDataSet.write(maxValsZ.data(), PredType::NATIVE_FLOAT);
        zMeanDataSet.write(meanValsZ.data(), PredType::NATIVE_FLOAT);
        zNanCountDataSet.write(nanValsZ.data(), PredType::NATIVE_INT64);
    }

    outputFile.close();
    rename(tempOutputFileName.c_str(), outputFileName.c_str());

    auto tEnd = chrono::high_resolution_clock::now();
    auto dtTotal = chrono::duration_cast<chrono::milliseconds>(tEnd - tStart).count();
    cout << "FITS file converted in " << dtTotal * 1e-3 << " seconds" << endl;
    delete[] standardCube;
    
    if (depth > 1) {
        delete[] rotatedCube;
    }
    return 0;
}
