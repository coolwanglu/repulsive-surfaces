#pragma once

#include <iostream>
#include <vector>
#include <sstream>
#include "rsurface_types.h"

namespace rsurfaces
{
    bool endsWith(std::string const &fullString, std::string const &ending);

    namespace scene
    {
        enum class ConstraintType
        {
            Barycenter,
            TotalArea,
            TotalVolume,
            BoundaryPins,
            VertexPins,
            BoundaryNormals,
            VertexNormals
        };

        enum class PotentialType
        {
            SquaredError,
            Area,
            Volume,
            SoftAreaConstraint,
            SoftVolumeConstraint
        };

        struct PotentialData
        {
            PotentialType type;
            double weight;
        };

        struct ConstraintData
        {
            ConstraintType type;
            double targetMultiplier;
            long numIterations;
        };

        struct ObstacleData
        {
            std::string obstacleName;
            double weight;
            bool recenter = false;
        };

        struct SceneData
        {
            std::string meshName;
            double alpha;
            double beta;
            bool allowBarycenterShift = false;
            std::vector<ObstacleData> obstacles;
            std::vector<ConstraintData> constraints;
            std::vector<PotentialData> potentials;
            std::vector<size_t> vertexPins;
            std::vector<size_t> vertexNormals;
            int iterationLimit = 0;
            long realTimeLimit = 0;
            std::string performanceLogFile = "performance.csv";
            GradientMethod defaultMethod = GradientMethod::HsProjectedIterative;
        };

        template <class Container>
        void splitString(const std::string &str, Container &cont, char delim = ' ')
        {
            std::stringstream ss(str);
            std::string token;
            while (std::getline(ss, token, delim))
            {
                cont.push_back(token);
            }
        }
        SceneData parseScene(std::string filename);

    } // namespace scene

    

} // namespace rsurfaces