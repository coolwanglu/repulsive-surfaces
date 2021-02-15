#include "main.h"
#include "main_picking.h"

#include "geometrycentral/surface/halfedge_mesh.h"
#include "geometrycentral/surface/meshio.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include "../deps/polyscope/deps/args/args/args.hxx"
#include "imgui.h"
#include "surface_derivatives.h"

#include "energy/tpe_kernel.h"
#include "energy/all_energies.h"
#include "helpers.h"
#include <memory>
#include "spatial/bvh_flattened.h"

#include <Eigen/Sparse>
#include <omp.h>

#include "sobolev/all_constraints.h"
#include "sobolev/hs.h"
#include "sobolev/hs_iterative.h"
#include "sobolev/h1.h"
#include "spatial/convolution.h"
#include "spatial/convolution_kernel.h"
#include "block_cluster_tree.h"
#include "surface_derivatives.h"
#include "obj_writer.h"
#include "dropdown_strings.h"
#include "energy/coulomb.h"

#include "bct_constructors.h"

#include "remeshing/remeshing.h"

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace rsurfaces
{

    MainApp *MainApp::instance = 0;

    MainApp::MainApp(MeshPtr mesh_, GeomPtr geom_, SurfaceFlow *flow_, polyscope::SurfaceMesh *psMesh_, std::string meshName_)
        : mesh(std::move(mesh_)), geom(std::move(geom_)), geomOrig(geom->copy()), remesher(mesh, geom, geomOrig)
    {
        flow = flow_;
        psMesh = psMesh_;
        meshName = meshName_;
        vertBVH = 0;
        vertexPotential = 0;
        ctrlMouseDown = false;
        hasPickedVertex = false;
        numSteps = 0;
        methodChoice = GradientMethod::HsProjectedIterative;
        timeSpentSoFar = 0;
        realTimeLimit = 0;
        logPerformance = false;
        referenceEnergy = 0;
        exitWhenDone = false;
    }

    void MainApp::logPerformanceLine()
    {
        if (!referenceEnergy)
        {
            referenceEnergy = new AllPairsTPEnergy(kernel);
        }

        geom->refreshQuantities();
        std::ofstream outfile;
        outfile.open(sceneData.performanceLogFile, std::ios_base::app);
        double currentEnergy = referenceEnergy->Value();
        std::cout << numSteps << ", " << timeSpentSoFar << ", " << currentEnergy << ", " << mesh->nFaces() << std::endl;
        outfile << numSteps << ", " << timeSpentSoFar << ", " << currentEnergy << ", " << mesh->nFaces() << std::endl;
        outfile.close();
    }

    void MainApp::TakeOptimizationStep(bool remeshAfter, bool showAreaRatios)
    {
        if (logPerformance && numSteps == 0)
        {
            logPerformanceLine();
        }

        long beforeStep = currentTimeMilliseconds();
        switch (methodChoice)
        {
        case GradientMethod::HsProjected:
            flow->StepProjectedGradient();
            break;
        case GradientMethod::HsProjectedIterative:
            flow->StepProjectedGradientIterative();
            break;
        case GradientMethod::HsNCG:
            flow->StepNCG();
            break;
        case GradientMethod::HsExactProjected:
            flow->StepProjectedGradientExact();
            break;
        case GradientMethod::H1Projected:
            flow->StepH1ProjGrad();
            break;
        case GradientMethod::L2Unconstrained:
            flow->StepL2Unconstrained();
            break;
        case GradientMethod::L2Projected:
            flow->StepL2Projected();
            break;
        case GradientMethod::AQP:
        {
            double kappa = 100;
            flow->StepAQP(1 / kappa);
        }
        break;
        case GradientMethod::H1_LBFGS:
            flow->StepH1LBFGS();
            break;
        case GradientMethod::BQN_LBFGS:
            flow->StepBQN();
            break;
        default:
            throw std::runtime_error("Unknown gradient method type.");
        }

        if (remeshAfter)
        {
            bool doCollapse = (numSteps % 1 == 0);
            std::cout << "Applying remeshing..." << std::endl;
            flow->verticesMutated = remesher.Remesh(5, doCollapse);
            if (flow->verticesMutated)
            {
                std::cout << "Vertices were mutated this step -- memory vectors are now invalid." << std::endl;
            }
            else
            {
                std::cout << "Vertices were not mutated this step." << std::endl;
            }

            mesh->compress();
            MainApp::instance->reregisterMesh();
        }
        else
        {
            flow->verticesMutated = false;
            MainApp::instance->updateMeshPositions();
        }
        long afterStep = currentTimeMilliseconds();
        long timeForStep = afterStep - beforeStep;
        timeSpentSoFar += timeForStep;
        numSteps++;
        std::cout << "  Mesh total volume = " << totalVolume(geom, mesh) << std::endl;
        std::cout << "  Mesh total area = " << totalArea(geom, mesh) << std::endl;

        if (logPerformance)
        {
            logPerformanceLine();
        }

        if (showAreaRatios)
        {
            VertexData<double> areaRatio(*mesh);
            for (Vertex v : mesh->vertices())
            {
                areaRatio[v] = geomOrig->vertexDualArea(v) / geom->vertexDualArea(v);
            }

            psMesh->addVertexScalarQuantity("Area ratios", areaRatio);
        }
    }

    void MainApp::updateMeshPositions()
    {
        if (normalizeView)
        {
            double scale = 0;
            for (GCVertex v : mesh->vertices())
            {
                scale = fmax(scale, norm(geom->inputVertexPositions[v]));
            }
            std::vector<Vector3> scaled(mesh->nVertices());
            VertexIndices inds = mesh->getVertexIndices();
            for (GCVertex v : mesh->vertices())
            {
                scaled[inds[v]] = geom->inputVertexPositions[v] / scale;
            }
            psMesh->updateVertexPositions(scaled);
        }
        else
        {
            psMesh->updateVertexPositions(geom->inputVertexPositions);
        }
        polyscope::requestRedraw();
    }

    void PlotMatrix(Eigen::MatrixXd &mat, polyscope::SurfaceMesh *psMesh, std::string name)
    {
        std::vector<Vector3> vecs;
        for (int i = 0; i < mat.rows(); i++)
        {
            Vector3 row_i = GetRow(mat, i);
            vecs.push_back(row_i);
        }
        psMesh->addVertexVectorQuantity(name, vecs);
    }

    void PlotVector(Eigen::VectorXd &vec, int nVerts, polyscope::SurfaceMesh *psMesh, std::string name)
    {
        Eigen::MatrixXd M;
        M.setZero(nVerts, 3);
        MatrixUtils::ColumnIntoMatrix(vec, M);
        PlotMatrix(M, psMesh, name);
    }

    void MainApp::PlotGradients()
    {
        Eigen::MatrixXd l2Diff, hsGrad, hsGradExact;
        l2Diff.setZero(mesh->nVertices(), 3);
        hsGrad.setZero(mesh->nVertices(), 3);
        hsGradExact.setZero(mesh->nVertices(), 3);

        flow->UpdateEnergies();

        std::cout << "Assembling L2 differential..." << std::endl;
        long diffTimeStart = currentTimeMilliseconds();
        flow->AssembleGradients(l2Diff);
        long diffTimeEnd = currentTimeMilliseconds();
        std::cout << "Differential took " << (diffTimeEnd - diffTimeStart) << " ms" << std::endl;

        std::unique_ptr<Hs::HsMetric> hs = flow->GetHsMetric();

        std::cout << "Inverting \"sparse\" metric..." << std::endl;
        long sparseTimeStart = currentTimeMilliseconds();
        hs->InvertMetricMat(l2Diff, hsGrad);
        long sparseTimeEnd = currentTimeMilliseconds();
        std::cout << "Sparse metric took " << (sparseTimeEnd - sparseTimeStart) << " ms" << std::endl;

        std::cout << "Inverting dense metric..." << std::endl;
        long timeStart = currentTimeMilliseconds();
        std::vector<ConstraintPack> empty;
        // hs->ProjectGradientExact(l2Diff, hsGradExact, empty);
        hsGradExact = hsGrad;
        long timeEnd = currentTimeMilliseconds();
        std::cout << "Dense metric took " << (timeEnd - timeStart) << " ms" << std::endl;

        PlotMatrix(l2Diff, psMesh, "L2 differential");
        PlotMatrix(hsGrad, psMesh, "Hs sparse gradient");
        PlotMatrix(hsGradExact, psMesh, "Hs dense gradient");
    }

    void MainApp::TestBarnesHut()
    {
        TPEKernel *tpe = new rsurfaces::TPEKernel(mesh, geom, 6, 12);
        SurfaceEnergy *energy_ap, *energy_bh, *energy_newton;
        energy_ap = new AllPairsTPEnergy(tpe);
        energy_bh = new BarnesHutTPEnergy6D(tpe, bh_theta);
        energy_newton = new BarnesHutNewtonian(tpe, bh_theta);

        energy_ap->Update();
        energy_bh->Update();
        energy_newton->Update();

        bool doAllPairs = (mesh->nFaces() < 3000);

        Eigen::MatrixXd grad_ap(mesh->nVertices(), 3);
        Eigen::MatrixXd grad_bh(mesh->nVertices(), 3);
        Eigen::MatrixXd grad_newton(mesh->nVertices(), 3);
        grad_ap.setZero();
        grad_bh.setZero();
        grad_newton.setZero();

        long start_ape = currentTimeMilliseconds();
        double value_ap = 0;
        if (doAllPairs)
            energy_ap->Value();
        long end_ape = currentTimeMilliseconds();

        long start_bhe = currentTimeMilliseconds();
        double value_bh = energy_bh->Value();
        long end_bhe = currentTimeMilliseconds();

        double val_error = fabs(value_ap - value_bh) / value_ap;

        if (!doAllPairs)
            std::cout << "Mesh has too many faces; not running all-pairs comparison." << std::endl;

        std::cout << "\n=====   Energy   =====" << std::endl;
        if (doAllPairs)
            std::cout << "All-pairs energy value  = " << value_ap << std::endl;
        std::cout << "Barnes-Hut energy value = " << value_bh << std::endl;
        if (doAllPairs)
            std::cout << "Relative error     = " << val_error * 100 << " percent" << std::endl;
        if (doAllPairs)
            std::cout << "All-pairs time     = " << (end_ape - start_ape) << " ms" << std::endl;
        std::cout << "Barnes-Hut time    = " << (end_bhe - start_bhe) << " ms" << std::endl;

        long start_apg = currentTimeMilliseconds();
        if (doAllPairs)
            energy_ap->Differential(grad_ap);
        long end_apg = currentTimeMilliseconds();

        long start_bhg = currentTimeMilliseconds();
        energy_bh->Differential(grad_bh);
        long end_bhg = currentTimeMilliseconds();

        long start_newton = currentTimeMilliseconds();
        energy_newton->Differential(grad_newton);
        long end_newton = currentTimeMilliseconds();

        double grad_error = (grad_ap - grad_bh).norm() / grad_ap.norm();

        std::cout << "\n=====  Gradient  =====" << std::endl;
        if (doAllPairs)
            std::cout << "All-pairs gradient norm      = " << grad_ap.norm() << std::endl;
        std::cout << "Barnes-Hut gradient norm     = " << grad_bh.norm() << std::endl;
        std::cout << "Newton gravity norm          = " << grad_newton.norm() << std::endl;
        if (doAllPairs)
            std::cout << "Barnes-Hut relative error    = " << grad_error * 100 << " percent" << std::endl;
        if (doAllPairs)
            std::cout << "All-pairs time     = " << (end_apg - start_apg) << " ms" << std::endl;
        std::cout << "Barnes-Hut time    = " << (end_bhg - start_bhg) << " ms" << std::endl;
        std::cout << "Newtonian time     = " << (end_newton - start_newton) << " ms" << std::endl;

        delete energy_ap;
        delete energy_bh;
        delete energy_newton;
        delete tpe;
    }

    void MainApp::PlotEnergyPerFace()
    {
        TPEKernel *tpe = new rsurfaces::TPEKernel(mesh, geom, 6, 12);
        BarnesHutTPEnergy6D *energy_bh = new BarnesHutTPEnergy6D(tpe, bh_theta);

        energy_bh->Update();
        double total = energy_bh->Value();

        for (GCFace f : mesh->faces())
        {
            double e = energy_bh->energyPerFace[f];
            // This looks like it scales the right way:
            // doubling the mesh also doubles the resulting lengths
            energy_bh->energyPerFace[f] = pow(e, 1.0 / (2 - tpe->alpha));
        }

        psMesh->addFaceScalarQuantity("energy per face", energy_bh->energyPerFace);
        std::cout << "Total energy = " << total << std::endl;

        delete energy_bh;
        delete tpe;
    }

    bool MainApp::pickNearbyVertex(GCVertex &out)
    {
        using namespace polyscope;
        Vector2 screenPos = getMouseScreenPos();

        std::pair<Structure *, size_t> pickVal =
            pick::evaluatePickQuery(screenPos.x, screenPos.y);

        GCVertex pickedVert;
        GCFace pickedFace;
        GCEdge pickedEdge;
        GCHalfedge pickedHalfedge;

        glm::mat4 view = polyscope::view::getCameraViewMatrix();
        glm::mat4 proj = polyscope::view::getCameraPerspectiveMatrix();
        glm::mat4 viewProj = proj * view;

        polyscope::SurfaceMesh *asMesh = dynamic_cast<polyscope::SurfaceMesh *>(pickVal.first);

        if (tryGetPickedVertex(asMesh, pickVal.second, mesh, pickedVert))
        {
            out = pickedVert;
            return true;
        }
        else if (tryGetPickedFace(asMesh, pickVal.second, mesh, pickedFace))
        {
            out = nearestVertexToScreenPos(screenPos, geom, viewProj, pickedFace);
            return true;
        }
        else if (tryGetPickedEdge(asMesh, pickVal.second, mesh, pickedEdge))
        {
            out = nearestVertexToScreenPos(screenPos, geom, viewProj, pickedEdge);
            return true;
        }
        else if (tryGetPickedHalfedge(asMesh, pickVal.second, mesh, pickedHalfedge))
        {
            out = nearestVertexToScreenPos(screenPos, geom, viewProj, pickedHalfedge);
            return true;
        }
        else
        {
            std::cout << "No valid element was picked (index " << pickVal.second << ")" << std::endl;
            return false;
        }
    }

    class PVCompare
    {
    public:
        bool operator()(PriorityVertex v1, PriorityVertex v2)
        {
            return (v1.priority > v2.priority);
        }
    };

    double gaussian(double radius, double dist)
    {
        double radterm = dist / radius;
        double epow = exp(-0.5 * radterm * radterm);
        return epow;
    }

    void MainApp::GetFalloffWindow(GCVertex v, double radius, std::vector<PriorityVertex> &verts)
    {
        // Do a simple Dijkstra search on edges
        VertexData<bool> seen(*mesh, false);
        std::priority_queue<PriorityVertex, std::vector<PriorityVertex>, PVCompare> queue;
        queue.push(PriorityVertex{v, 0, geom->inputVertexPositions[v]});

        while (!queue.empty())
        {
            PriorityVertex next = queue.top();
            queue.pop();

            if (next.priority > radius)
            {
                break;
            }
            else if (seen[next.vertex])
            {
                continue;
            }
            else
            {
                // Mark the next vertex as seen
                seen[next.vertex] = true;
                // Compute the weight
                double weight = gaussian(radius / 3, next.priority);
                verts.push_back(PriorityVertex{next.vertex, weight, geom->inputVertexPositions[next.vertex]});

                // Enqueue all neighbors
                for (GCVertex neighbor : next.vertex.adjacentVertices())
                {
                    if (seen[neighbor])
                    {
                        continue;
                    }
                    // Add the next edge distance
                    Vector3 p1 = geom->inputVertexPositions[next.vertex];
                    Vector3 p2 = geom->inputVertexPositions[neighbor];
                    double neighborDist = next.priority + norm(p1 - p2);

                    queue.push(PriorityVertex{neighbor, neighborDist, geom->inputVertexPositions[neighbor]});
                }
            }
        }

        std::cout << "Got " << verts.size() << " vertices" << std::endl;
    }

    void MainApp::HandlePicking()
    {
        using namespace polyscope;

        auto io = ImGui::GetIO();
        glm::mat4 view = polyscope::view::getCameraViewMatrix();
        glm::mat4 proj = polyscope::view::getCameraPerspectiveMatrix();
        glm::mat4 viewProj = proj * view;

        if (io.KeyCtrl && io.MouseDown[0])
        {
            if (!ctrlMouseDown)
            {
                if (pickNearbyVertex(pickedVertex))
                {
                    hasPickedVertex = true;
                    GetFalloffWindow(pickedVertex, 0.5, dragVertices);

                    Vector3 screen = projectToScreenCoords3(geom->inputVertexPositions[pickedVertex], viewProj);
                    pickDepth = screen.z;

                    Vector3 unprojected = unprojectFromScreenCoords3(Vector2{screen.x, screen.y}, pickDepth, viewProj);
                    initialPickedPosition = geom->inputVertexPositions[pickedVertex];
                }
                ctrlMouseDown = true;
            }
            else
            {
                if (hasPickedVertex)
                {
                    Vector2 mousePos = getMouseScreenPos();
                    Vector3 unprojected = unprojectFromScreenCoords3(mousePos, pickDepth, viewProj);
                    Vector3 displacement = unprojected - initialPickedPosition;

                    for (PriorityVertex &v : dragVertices)
                    {
                        Vector3 newPos = v.position + v.priority * displacement;
                        geom->inputVertexPositions[v.vertex] = newPos;
                    }

                    flow->ResetAllConstraints();
                    flow->ResetAllPotentials();

                    if (vertexPotential)
                    {
                        for (PriorityVertex &v : dragVertices)
                        {
                            vertexPotential->ChangeVertexTarget(v.vertex, geom->inputVertexPositions[v.vertex]);
                        }
                    }

                    updateMeshPositions();
                }
            }
        }
        else
        {
            if (ctrlMouseDown)
            {
                ctrlMouseDown = false;
                hasPickedVertex = false;
                dragVertices.clear();
                // geom->inputVertexPositions[pickedVertex] = initialPickedPosition;
                updateMeshPositions();
            }
        }
    }

    void MainApp::Scale2x()
    {
        for (GCVertex v : mesh->vertices())
        {
            geom->inputVertexPositions[v] = 2 * geom->inputVertexPositions[v];
        }
    }

    Jacobian numericalNormalDeriv(GeomPtr &geom, GCVertex vert, GCVertex wrt)
    {
        double h = 1e-4;

        Vector3 origNormal = vertexAreaNormal(geom, vert);
        Vector3 origPos = geom->inputVertexPositions[wrt];
        geom->inputVertexPositions[wrt] = origPos + Vector3{h, 0, 0};
        geom->refreshQuantities();
        Vector3 n_x = vertexAreaNormal(geom, vert);

        geom->inputVertexPositions[wrt] = origPos + Vector3{0, h, 0};
        geom->refreshQuantities();
        Vector3 n_y = vertexAreaNormal(geom, vert);

        geom->inputVertexPositions[wrt] = origPos + Vector3{0, 0, h};
        geom->refreshQuantities();
        Vector3 n_z = vertexAreaNormal(geom, vert);

        geom->inputVertexPositions[wrt] = origPos;
        geom->refreshQuantities();

        Vector3 deriv_y = (n_y - origNormal) / h;
        Vector3 deriv_z = (n_z - origNormal) / h;
        Vector3 deriv_x = (n_x - origNormal) / h;
        Jacobian J_num{deriv_x, deriv_y, deriv_z};
        return J_num;
    }

    void MainApp::TestNormalDeriv()
    {
        GCVertex vert;
        for (GCVertex v : mesh->vertices())
        {
            if (v.isBoundary())
            {
                vert = v;
                break;
            }
        }
        std::cout << "Testing vertex " << vert << std::endl;
        for (GCVertex neighbor : vert.adjacentVertices())
        {
            std::cout << "Derivative of normal of " << vert << " wrt " << neighbor << std::endl;
            Jacobian dWrtNeighbor = SurfaceDerivs::vertexNormalWrtVertex(geom, vert, neighbor);
            dWrtNeighbor.Print();
            std::cout << "Numerical:" << std::endl;
            numericalNormalDeriv(geom, vert, neighbor).Print();
        }
        std::cout << "Derivative of normal of " << vert << " wrt " << vert << std::endl;
        Jacobian dWrtSelf = SurfaceDerivs::vertexNormalWrtVertex(geom, vert, vert);
        dWrtSelf.Print();
        std::cout << "Numerical:" << std::endl;
        numericalNormalDeriv(geom, vert, vert).Print();
    }

    void MainApp::TestNewMVProduct()
    {
        auto mesh = rsurfaces::MainApp::instance->mesh;
        auto geom = rsurfaces::MainApp::instance->geom;
        size_t nVertices = mesh->nVertices();

        // Use the differential of the energy as the test case
        SurfaceEnergy *energy = flow->BaseEnergy();
        energy->Update();
        Eigen::MatrixXd gradient(nVertices, 3);
        energy->Differential(gradient);
        Eigen::VectorXd gVec(3 * nVertices);
        MatrixUtils::MatrixIntoColumn(gradient, gVec);
        Vector2 exps = energy->GetExponents();
        double s = 2 - Hs::get_s(exps.x, exps.y);

        Hs::HsMetric hs(energy);
        std::cout << "Dense fractional Laplacian (s = " << s << "):" << std::endl;
        Eigen::MatrixXd dense, dense_small;
        dense_small.setZero(nVertices, nVertices);
        dense.setZero(3 * nVertices, 3 * nVertices);
        // Build dense fractional Laplacian
        long fracStart = currentTimeMilliseconds();
        hs.FillMatrixFracOnly(dense_small, s, mesh, geom);
        MatrixUtils::TripleMatrix(dense_small, dense);
        // Multiply it
        Eigen::VectorXd denseRes = dense * gVec;

        long constructStart = currentTimeMilliseconds();

        BlockClusterTree2 *bct = CreateOptimizedBCT(mesh, geom, exps.x, exps.y, 0.5);

        long constructEnd = currentTimeMilliseconds();

        Eigen::VectorXd fastRes;
        fastRes.setZero(nVertices * 3);
        bct->MultiplyV3(gVec, fastRes, BCTKernelType::FractionalOnly);

        long multiplyEnd = currentTimeMilliseconds();

        std::cout << "Construction time = " << (constructEnd - constructStart) << " ms" << std::endl;
        std::cout << "Multiplication time = " << (multiplyEnd - constructEnd) << " ms" << std::endl;

        Eigen::VectorXd diff = denseRes - fastRes;
        double error = 100 * diff.norm() / denseRes.norm();
        std::cout << "Dense multiply norm = " << denseRes.norm() << std::endl;
        std::cout << "Hierarchical multiply norm = " << fastRes.norm() << std::endl;
        std::cout << "Relative error = " << error << " percent" << std::endl;
        std::cout << "Dot product of directions = " << denseRes.dot(fastRes) / (denseRes.norm() * fastRes.norm()) << std::endl;

        // Reset for high-order term
        denseRes.setZero();
        fastRes.setZero();
        dense_small.setZero();
        dense.setZero();
        s = Hs::get_s(exps.x, exps.y);

        std::cout << "High-order term (s = " << s << "):" << std::endl;

        // Build dense matrix for high-order term
        long hiStart = currentTimeMilliseconds();
        hs.FillMatrixHigh(dense_small, s, mesh, geom);
        MatrixUtils::TripleMatrix(dense_small, dense);
        // Multiply it
        denseRes = dense * gVec;
        long hiEnd = currentTimeMilliseconds();

        bct->MultiplyV3(gVec, fastRes, BCTKernelType::HighOrder);
        std::cout << "Multiplication time = " << (multiplyEnd - constructEnd) << " ms" << std::endl;

        diff = denseRes - fastRes;
        error = 100 * diff.norm() / denseRes.norm();
        std::cout << "Dense multiply norm = " << denseRes.norm() << std::endl;
        std::cout << "Hierarchical multiply norm = " << fastRes.norm() << std::endl;
        std::cout << "Relative error = " << error << " percent" << std::endl;
        std::cout << "Dot product of directions = " << denseRes.dot(fastRes) / (denseRes.norm() * fastRes.norm()) << std::endl;

        delete bct;
    } // TestNewMVProduct

    void MainApp::TestMVProduct()
    {
        geom->refreshQuantities();

        long gradientStartTime = currentTimeMilliseconds();
        // Use the differential of the energy as the test case
        SurfaceEnergy *energy = flow->BaseEnergy();
        energy->Update();
        Eigen::MatrixXd gradient(mesh->nVertices(), 3);
        energy->Differential(gradient);
        Eigen::VectorXd gVec(3 * mesh->nVertices());
        MatrixUtils::MatrixIntoColumn(gradient, gVec);
        Vector2 exps = energy->GetExponents();
        double s = 2 - Hs::get_s(exps.x, exps.y);

        long gradientEndTime = currentTimeMilliseconds();
        Hs::HsMetric hs(energy);

        std::cout << "Dense fractional Laplacian (s = " << s << "):" << std::endl;
        Eigen::MatrixXd dense, dense_small;
        dense_small.setZero(mesh->nVertices(), mesh->nVertices());
        dense.setZero(3 * mesh->nVertices(), 3 * mesh->nVertices());
        // Build dense fractional Laplacian
        long fracStart = currentTimeMilliseconds();
        hs.FillMatrixFracOnly(dense_small, s, mesh, geom);
        MatrixUtils::TripleMatrix(dense_small, dense);
        // Multiply it
        Eigen::VectorXd denseRes = dense * gVec;
        long fracEnd = currentTimeMilliseconds();

        // Block cluster tree multiplication
        BVHNode6D *bvh = energy->GetBVH();

        BlockClusterTree *bct = new BlockClusterTree(mesh, geom, bvh, bh_theta, s);
        long fracBCT = currentTimeMilliseconds();
        Eigen::VectorXd bctRes = gVec;
        bct->MultiplyVector3(gVec, bctRes, BCTKernelType::FractionalOnly);
        long fracEnd2 = currentTimeMilliseconds();

        bct->PrintData();

        Eigen::VectorXd diff = denseRes - bctRes;
        double error = 100 * diff.norm() / denseRes.norm();
        std::cout << "Dense multiply norm = " << denseRes.norm() << std::endl;
        std::cout << "Hierarchical multiply norm = " << bctRes.norm() << std::endl;
        std::cout << "Relative error = " << error << " percent" << std::endl;
        std::cout << "Dot product of directions = " << denseRes.dot(bctRes) / (denseRes.norm() * bctRes.norm()) << std::endl;
        std::cout << "Dense assembly took " << (fracEnd - fracStart) << " ms, hierarchical product took " << (fracEnd2 - fracBCT) << " ms" << std::endl;
        std::cout << "BCT assembly took " << (fracBCT - fracEnd) << " ms" << std::endl;

        // Reset for high-order term
        denseRes.setZero();
        bctRes.setZero();
        dense_small.setZero();
        dense.setZero();
        s = Hs::get_s(exps.x, exps.y);
        bct->SetExponent(s);

        std::cout << "High-order term (s = " << s << "):" << std::endl;

        // Build dense matrix for high-order term
        long hiStart = currentTimeMilliseconds();
        hs.FillMatrixHigh(dense_small, s, mesh, geom);
        MatrixUtils::TripleMatrix(dense_small, dense);
        // Multiply it
        denseRes = dense * gVec;
        long hiEnd = currentTimeMilliseconds();

        // Use BCT to multiply high-order term
        bct->MultiplyVector3(gVec, bctRes, BCTKernelType::HighOrder);
        long hiEnd2 = currentTimeMilliseconds();

        diff = denseRes - bctRes;
        error = 100 * diff.norm() / denseRes.norm();
        std::cout << "Dense multiply norm = " << denseRes.norm() << std::endl;
        std::cout << "Hierarchical multiply norm = " << bctRes.norm() << std::endl;
        std::cout << "Relative error = " << error << " percent" << std::endl;
        std::cout << "Dot product of directions = " << denseRes.dot(bctRes) / (denseRes.norm() * bctRes.norm()) << std::endl;
        std::cout << "Dense assembly took " << (hiEnd - hiStart) << " ms, hierarchical product took " << (hiEnd2 - hiEnd) << " ms" << std::endl;

        // Reset for low-order term
        denseRes.setZero();
        bctRes.setZero();
        dense_small.setZero();
        dense.setZero();
        s = Hs::get_s(exps.x, exps.y);
        bct->SetExponent(s);

        std::cout << "Low-order term (s = " << s << "):" << std::endl;

        // Build dense matrix for low-order term
        long lowStart = currentTimeMilliseconds();
        hs.FillMatrixLow(dense_small, s, mesh, geom);
        MatrixUtils::TripleMatrix(dense_small, dense);
        // Multiply it
        denseRes = dense * gVec;
        long lowEnd = currentTimeMilliseconds();

        // Use BCT to multiply low-order term
        bct->MultiplyVector3(gVec, bctRes, BCTKernelType::LowOrder);
        long lowEnd2 = currentTimeMilliseconds();

        diff = denseRes - bctRes;
        error = 100 * diff.norm() / denseRes.norm();
        std::cout << "Dense multiply norm = " << denseRes.norm() << std::endl;
        std::cout << "Hierarchical multiply norm = " << bctRes.norm() << std::endl;
        std::cout << "Relative error = " << error << " percent" << std::endl;
        std::cout << "Dot product of directions = " << denseRes.dot(bctRes) / (denseRes.norm() * bctRes.norm()) << std::endl;
        std::cout << "Dense assembly took " << (lowEnd - lowStart) << " ms, hierarchical product took " << (lowEnd2 - lowEnd) << " ms" << std::endl;

        delete bct;
    }

    void MainApp::TestIterative()
    {
        geom->refreshQuantities();

        // Constraints::TotalAreaConstraint *c = new Constraints::TotalAreaConstraint(mesh, geom);
        // schurConstraints.push_back(ConstraintPack{c, 0, 0});

        long gradientStartTime = currentTimeMilliseconds();
        // Use the differential of the energy as the test case
        SurfaceEnergy *energy = flow->BaseEnergy();
        energy->Update();
        Eigen::MatrixXd gradient(mesh->nVertices(), 3);
        energy->Differential(gradient);
        long gradientEndTime = currentTimeMilliseconds();

        Vector3 avg = averageOfMatrixRows(geom, mesh, gradient);

        Hs::HsMetric hs(energy);

        Eigen::VectorXd gVec;
        gVec.setZero(hs.getNumRows());
        MatrixUtils::MatrixIntoColumn(gradient, gVec);

        std::cout << "Extended gradient vector has " << gVec.rows() << " rows" << std::endl;

        Eigen::VectorXd denseRes = gVec;
        denseRes.setZero();
        Eigen::VectorXd iterativeRes = denseRes;

        std::cout << "Projecting using dense system..." << std::endl;
        long denseStart = currentTimeMilliseconds();
        Eigen::MatrixXd dense = hs.GetHsMatrixConstrained(false);
        std::cout << "Computed dense matrix" << std::endl;
        MatrixUtils::SolveDenseSystem(dense, gVec, denseRes);
        long denseEnd = currentTimeMilliseconds();
        std::cout << "Finished in " << (denseEnd - denseStart) << " ms." << std::endl;

        std::cout << "Projecting using iterative method..." << std::endl;
        iterativeRes.setZero();
        long iterStart = currentTimeMilliseconds();
        Hs::ProjectUnconstrainedHsIterative(hs, gVec, iterativeRes);
        long iterEnd = currentTimeMilliseconds();
        std::cout << "Finished in " << (iterEnd - iterStart) << " ms." << std::endl;

        Eigen::VectorXd diff = iterativeRes - denseRes;
        double error = diff.norm() / denseRes.norm() * 100;

        std::cout << "Dense norm = " << denseRes.norm() << std::endl;
        std::cout << "Iterative norm = " << iterativeRes.norm() << std::endl;
        std::cout << "Relative error = " << error << " percent" << std::endl;
        std::cout << "Dot product of directions = " << denseRes.dot(iterativeRes) / (denseRes.norm() * iterativeRes.norm()) << std::endl;

        std::cout << "Computed gradient in " << (gradientEndTime - gradientStartTime) << " ms" << std::endl;
    }

    class VectorInit
    {
    public:
        static void Init(Vector3 &data, BVHNode6D *node)
        {
            data = Vector3{1, 2, 3};
        }
    };

    void MainApp::BenchmarkBH()
    {
        SurfaceEnergy *energy = flow->BaseEnergy();
        Eigen::MatrixXd diff(mesh->nVertices(), 3);

        long totalBVH = 0, totalE = 0, totalG = 0;

        const int nTrials = 100;

        for (int i = 0; i < nTrials; i++)
        {
            diff.setZero();

            long bvhStart = currentTimeMilliseconds();
            energy->Update();
            long eStart = currentTimeMilliseconds();
            double eVal = energy->Value();
            long mid = currentTimeMilliseconds();
            energy->Differential(diff);
            long gEnd = currentTimeMilliseconds();

            long bvhTime = (eStart - bvhStart);
            long eTime = (mid - eStart);
            long gTime = (gEnd - mid);

            totalBVH += bvhTime;
            totalE += eTime;
            totalG += gTime;

            std::cout << i << ": BVH " << bvhTime << " ms, energy " << eTime << " ms, gradient " << gTime << " ms" << std::endl;
        }

        std::cout << "Average over " << nTrials << " runs:" << std::endl;
        std::cout << "BVH construction:    " << ((double)totalBVH / nTrials) << " ms" << std::endl;
        std::cout << "Energy evaluation:   " << ((double)totalE / nTrials) << " ms" << std::endl;
        std::cout << "Gradient evaluation: " << ((double)totalG / nTrials) << " ms" << std::endl;
    }

    void MainApp::AddObstacle(std::string filename, double weight, bool recenter)
    {
        std::unique_ptr<HalfedgeMesh> obstacleMesh;
        std::unique_ptr<VertexPositionGeometry> obstacleGeometry;
        // Load mesh
        std::tie(obstacleMesh, obstacleGeometry) = loadMesh(filename);

        obstacleGeometry->requireVertexDualAreas();
        obstacleGeometry->requireVertexNormals();

        if (recenter)
        {
            Vector3 obstacleCenter = meshBarycenter(obstacleGeometry, obstacleMesh);
            std::cout << "Recentering obstacle " << filename << " (offset " << obstacleCenter << ")" << std::endl;
            for (GCVertex v : obstacleMesh->vertices())
            {
                obstacleGeometry->inputVertexPositions[v] = obstacleGeometry->inputVertexPositions[v] - obstacleCenter;
            }
        }

        std::string mesh_name = polyscope::guessNiceNameFromPath(filename);
        polyscope::SurfaceMesh *psMesh = polyscope::registerSurfaceMesh(mesh_name, obstacleGeometry->inputVertexPositions,
                                                                        obstacleMesh->getFaceVertexList(), polyscopePermutations(*obstacleMesh));

        double exp = kernel->beta - kernel->alpha;
        StaticObstacle *obstacle = new StaticObstacle(mesh, geom, std::move(obstacleMesh), std::move(obstacleGeometry), exp, bh_theta, weight);
        flow->AddAdditionalEnergy(obstacle);
        std::cout << "Added " << filename << " as obstacle with weight " << weight << std::endl;
    }

    void MainApp::AddImplicitBarrier(scene::ImplicitBarrierData &barrierData)
    {
        ImplicitSurface* implSurface;
        // Create the requested implicit surface
        switch (barrierData.type)
        {
            case scene::ImplicitType::Plane:
            {
                Vector3 point{barrierData.parameters[0], barrierData.parameters[1], barrierData.parameters[2]};
                Vector3 normal{barrierData.parameters[3], barrierData.parameters[4], barrierData.parameters[5]};
                std::cout << "Constructing implicit plane at point " << point << " with normal " << normal << std::endl;
                implSurface = new FlatPlane(point, normal);
            }
            break;
            case scene::ImplicitType::Torus:
            {
                double major = barrierData.parameters[0];
                double minor = barrierData.parameters[1];
                Vector3 center{barrierData.parameters[2], barrierData.parameters[3], barrierData.parameters[4]};
                std::cout << "Constructing implicit torus with major radius " << major << ", minor radius " << minor << ", center " << center << std::endl;
                implSurface = new ImplicitTorus(major, minor, center);
            }
            break;
            case scene::ImplicitType::Sphere:
            {
                double radius = barrierData.parameters[0];
                Vector3 center{barrierData.parameters[1], barrierData.parameters[2], barrierData.parameters[3]};
                std::cout << "Constructing implicit sphere with radius " << radius << ", center " << center << std::endl;
                implSurface = new ImplicitSphere(radius, center);
            }
            break;
            default:
            {
                throw std::runtime_error("Unimplemented implicit surface type.");
            }
            break;
        }

        // Mesh the 0 isosurface so we can see the implicit surface
        MainApp::instance->MeshImplicitSurface(implSurface);

        // Use the implicit surface to setup the energy
        std::unique_ptr<ImplicitSurface> implUnique(implSurface);
        if (barrierData.repel)
        {
            std::cout << "Using implicit surface as obstacle, with weight " << barrierData.weight << std::endl;
            ImplicitObstacle* obstacle = new ImplicitObstacle(mesh, geom, std::move(implUnique), barrierData.weight);
            flow->AddAdditionalEnergy(obstacle);
        }
        else
        {
            std::cout << "Using implicit surface as attractor, with weight " << barrierData.weight << std::endl;
            ImplicitAttractor* attractor = new ImplicitAttractor(mesh, geom, std::move(implUnique), barrierData.weight);
            flow->AddAdditionalEnergy(attractor);
        }
    }

    void MainApp::AddPotential(scene::PotentialType pType, double weight)
    {
        switch (pType)
        {
        case scene::PotentialType::SquaredError:
        {
            SquaredError *errorPotential = new SquaredError(mesh, geom, weight);
            vertexPotential = errorPotential;
            flow->AddAdditionalEnergy(errorPotential);
            remesher.KeepVertexDataUpdated(&errorPotential->originalPositions);
            break;
        }
        case scene::PotentialType::Area:
        {
            TotalAreaPotential *areaPotential = new TotalAreaPotential(mesh, geom, weight);
            flow->AddAdditionalEnergy(areaPotential);
            break;
        }
        case scene::PotentialType::Volume:
        {
            TotalVolumePotential *volumePotential = new TotalVolumePotential(mesh, geom, weight);
            flow->AddAdditionalEnergy(volumePotential);
            break;
        }
        case scene::PotentialType::SoftAreaConstraint:
        {
            SoftAreaConstraint *softArea = new SoftAreaConstraint(mesh, geom, weight);
            flow->AddAdditionalEnergy(softArea);
            break;
        }
        case scene::PotentialType::SoftVolumeConstraint:
        {
            SoftVolumeConstraint *softVol = new SoftVolumeConstraint(mesh, geom, weight);
            flow->AddAdditionalEnergy(softVol);
            break;
        }
        default:
        {
            std::cout << "Unknown potential type." << std::endl;
            break;
        }
        }
    }

    void MainApp::MeshImplicitSurface(ImplicitSurface *surface)
    {
        CIsoSurface<double> *iso = new CIsoSurface<double>();

        std::cout << "Meshing the supplied implicit surface using marching cubes..." << std::endl;

        const int numCells = 50;
        Vector3 center = surface->BoundingCenter();
        double diameter = surface->BoundingDiameter();
        double cellSize = diameter / numCells;
        double radius = diameter / 2;

        Vector3 lowerCorner = center - Vector3{radius, radius, radius};

        int numCorners = numCells + 1;

        double field[numCorners * numCorners * numCorners];

        int nSlice = numCorners * numCorners;
        int nRow = numCorners;

        for (int x = 0; x < numCorners; x++)
        {
            for (int y = 0; y < numCorners; y++)
            {
                for (int z = 0; z < numCorners; z++)
                {
                    Vector3 samplePt = lowerCorner + Vector3{(double)x, (double)y, (double)z} * cellSize;
                    double value = surface->SignedDistance(samplePt);
                    field[nSlice * z + nRow * y + x] = value;
                }
            }
        }

        iso->GenerateSurface(field, 0, numCells, numCells, numCells, cellSize, cellSize, cellSize);

        std::vector<glm::vec3> nodes;
        std::vector<std::array<size_t, 3>> triangles;

        int nVerts = iso->m_nVertices;

        for (int i = 0; i < nVerts; i++)
        {
            double x = iso->m_ppt3dVertices[i][0];
            double y = iso->m_ppt3dVertices[i][1];
            double z = iso->m_ppt3dVertices[i][2];

            Vector3 p = lowerCorner + Vector3{x, y, z};
            nodes.push_back(glm::vec3{p.x, p.y, p.z});
        }

        int nTris = iso->m_nTriangles;

        for (int i = 0; i < nTris; i++)
        {
            int i1 = iso->m_piTriangleIndices[3 * i];
            int i2 = iso->m_piTriangleIndices[3 * i + 1];
            int i3 = iso->m_piTriangleIndices[3 * i + 2];

            triangles.push_back({(size_t)i1, (size_t)i2, (size_t)i3});
        }

        polyscope::registerSurfaceMesh("implicitSurface", nodes, triangles);
        delete iso;
    }
} // namespace rsurfaces

// UI parameters
bool run = false;
bool takeScreenshots = false;
bool saveOBJs = false;
uint screenshotNum = 0;
uint objNum = 0;
bool uiNormalizeView = false;
bool remesh = true;
bool changeTopo = false;
bool areaRatios = false;

int partIndex = 4475;

void saveScreenshot(uint i)
{
    char buffer[5];
    std::snprintf(buffer, sizeof(buffer), "%04d", i);
    std::string fname = "frames/frame" + std::string(buffer) + ".png";
    polyscope::screenshot(fname, false);
    std::cout << "Saved screenshot to " << fname << std::endl;
}

void saveOBJ(rsurfaces::MeshPtr mesh, rsurfaces::GeomPtr geom, rsurfaces::GeomPtr geomOrig, uint i)
{

    char buffer[5];
    std::snprintf(buffer, sizeof(buffer), "%04d", i);
    std::string fname = "objs/frame" + std::string(buffer) + ".obj";
    rsurfaces::writeMeshToOBJ(mesh, geom, geomOrig, areaRatios, fname);
    std::cout << "Saved OBJ frame to " << fname << std::endl;
}

template <typename ItemType>
void selectFromDropdown(std::string label, const ItemType choices[], size_t nChoices, ItemType &store)
{
    using namespace rsurfaces;

    // Dropdown menu for list of remeshing mode settings
    if (ImGui::BeginCombo(label.c_str(), StringOfMode(store).c_str()))
    {
        for (size_t i = 0; i < nChoices; i++)
        {
            bool is_selected = (store == choices[i]);
            if (ImGui::Selectable(StringOfMode(choices[i]).c_str(), is_selected))
                store = choices[i];
            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// A user-defined callback, for creating control panels (etc)
// Use ImGUI commands to build whatever you want here, see
// https://github.com/ocornut/imgui/blob/master/imgui.h
void customCallback()
{
    using namespace rsurfaces;

    const int INDENT = 10;
    const int ITEM_WIDTH = 160;

    ImGui::Text("Flow control");
    ImGui::BeginGroup();
    ImGui::Indent(INDENT);
    ImGui::PushItemWidth(ITEM_WIDTH);
    ImGui::Checkbox("Run flow", &run);
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    ImGui::Checkbox("Normalize view", &uiNormalizeView);

    ImGui::Checkbox("Take screenshots", &takeScreenshots);
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    if ((takeScreenshots && screenshotNum == 0) || ImGui::Button("Take screenshot", ImVec2{ITEM_WIDTH, 0}))
    {
        saveScreenshot(screenshotNum++);
    }

    ImGui::Checkbox("Write OBJs", &saveOBJs);
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    if ((saveOBJs && objNum == 0) || ImGui::Button("Write OBJ", ImVec2{ITEM_WIDTH, 0}))
    {
        saveOBJ(MainApp::instance->mesh, MainApp::instance->geom, MainApp::instance->geomOrig, objNum++);
    }
    ImGui::Checkbox("Log performance", &MainApp::instance->logPerformance);
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    ImGui::Checkbox("Show area ratios", &areaRatios);

    const GradientMethod methods[] = {GradientMethod::HsProjectedIterative,
                                      GradientMethod::HsProjected,
                                      GradientMethod::HsExactProjected,
                                      GradientMethod::H1Projected,
                                      GradientMethod::L2Unconstrained,
                                      GradientMethod::L2Projected,
                                      GradientMethod::AQP,
                                      GradientMethod::H1_LBFGS,
                                      GradientMethod::BQN_LBFGS};

    selectFromDropdown("Method", methods, IM_ARRAYSIZE(methods), MainApp::instance->methodChoice);

    ImGui::Checkbox("Dynamic remeshing", &remesh);

    const remeshing::RemeshingMode rModes[] = {remeshing::RemeshingMode::FlipOnly,
                                               remeshing::RemeshingMode::SmoothAndFlip,
                                               remeshing::RemeshingMode::SmoothFlipAndCollapse};

    const remeshing::SmoothingMode sModes[] = {remeshing::SmoothingMode::Laplacian,
                                               remeshing::SmoothingMode::Circumcenter};

    const remeshing::FlippingMode fModes[] = {remeshing::FlippingMode::Delaunay,
                                              remeshing::FlippingMode::Degree};

    selectFromDropdown("Remeshing mode", rModes, IM_ARRAYSIZE(rModes), MainApp::instance->remesher.remeshingMode);
    selectFromDropdown("Smoothing mode", sModes, IM_ARRAYSIZE(sModes), MainApp::instance->remesher.smoothingMode);
    selectFromDropdown("Flipping mode", fModes, IM_ARRAYSIZE(fModes), MainApp::instance->remesher.flippingMode);

    ImGui::Checkbox("Curvature adaptive remeshing", &MainApp::instance->remesher.curvatureAdaptive);

    rsurfaces::MainApp::instance->HandlePicking();

    ImGui::InputInt("Iteration limit", &MainApp::instance->stepLimit);
    ImGui::InputInt("Real time limit (ms)", &MainApp::instance->realTimeLimit);

    if (uiNormalizeView != MainApp::instance->normalizeView)
    {
        rsurfaces::MainApp::instance->normalizeView = uiNormalizeView;
        rsurfaces::MainApp::instance->updateMeshPositions();
    }
    ImGui::PopItemWidth();
    if (ImGui::Button("Take 1 step", ImVec2{ITEM_WIDTH, 0}) || run)
    {
        MainApp::instance->TakeOptimizationStep(remesh, areaRatios);

        if (takeScreenshots)
        {
            saveScreenshot(screenshotNum++);
        }
        if (saveOBJs)
        {
            saveOBJ(MainApp::instance->mesh, MainApp::instance->geom, MainApp::instance->geomOrig, objNum++);
        }
        if ((MainApp::instance->stepLimit > 0 && MainApp::instance->numSteps >= MainApp::instance->stepLimit) ||
            (MainApp::instance->realTimeLimit > 0 && MainApp::instance->timeSpentSoFar >= MainApp::instance->realTimeLimit))
        {
            run = false;
            if (MainApp::instance->exitWhenDone)
            {
                std::exit(0);
            }
        }
    }

    ImGui::EndGroup();

    ImGui::Text("Accuracy tests");

    ImGui::BeginGroup();
    ImGui::Indent(INDENT);

    if (ImGui::Button("Test new MV product", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->TestNewMVProduct();
    }

    if (ImGui::Button("Test MV product", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->TestMVProduct();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    if (ImGui::Button("Test iterative", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->TestIterative();
    }

    if (ImGui::Button("Test B-H", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->TestBarnesHut();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    if (ImGui::Button("Benchmark B-H", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->BenchmarkBH();
    }

    if (ImGui::Button("Plot face energies", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->PlotEnergyPerFace();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    if (ImGui::Button("Scale mesh 2x", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->Scale2x();
    }

    if (ImGui::Button("Plot gradients", ImVec2{ITEM_WIDTH, 0}))
    {
        MainApp::instance->PlotGradients();
    }
    ImGui::EndGroup();

    ImGui::Text("Remeshing tests");

    ImGui::BeginGroup();
    ImGui::Indent(INDENT);

    // Section for remeshing tests
    if (ImGui::Button("Fix Delaunay"))
    {
        remeshing::fixDelaunay(MainApp::instance->mesh, MainApp::instance->geom);
        MainApp::instance->mesh->validateConnectivity();
        MainApp::instance->reregisterMesh();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);

    if (ImGui::Button("Laplacian smooth"))
    {
        remeshing::smoothByLaplacian(MainApp::instance->mesh, MainApp::instance->geom);
        MainApp::instance->reregisterMesh();
    }

    if (ImGui::Button("Circumcenter smooth"))
    {
        remeshing::smoothByCircumcenter(MainApp::instance->mesh, MainApp::instance->geom);
        MainApp::instance->reregisterMesh();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);

    if (ImGui::Button("Laplacian opt"))
    {
        for (int i = 0; i < 10; i++)
        {
            remeshing::smoothByLaplacian(MainApp::instance->mesh, MainApp::instance->geom);
            remeshing::fixDelaunay(MainApp::instance->mesh, MainApp::instance->geom);
        }
        MainApp::instance->reregisterMesh();
    }

    if (ImGui::Button("Circumcenter opt"))
    {
        for (int i = 0; i < 10; i++)
        {
            remeshing::smoothByCircumcenter(MainApp::instance->mesh, MainApp::instance->geom);
            remeshing::fixDelaunay(MainApp::instance->mesh, MainApp::instance->geom);
        }
        MainApp::instance->reregisterMesh();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);
    if (ImGui::Button("Adjust edge lengths"))
    {
        remeshing::adjustEdgeLengths(MainApp::instance->mesh, MainApp::instance->geom, MainApp::instance->geomOrig, 0.01, 0.1, 0.001);
        MainApp::instance->reregisterMesh();
    }

    if (ImGui::Button("Adjust vert degrees"))
    {
        remeshing::adjustVertexDegrees(MainApp::instance->mesh, MainApp::instance->geom);
        MainApp::instance->reregisterMesh();
    }
    ImGui::SameLine(ITEM_WIDTH, 2 * INDENT);

    if (ImGui::Button("Face Weight Smooth"))
    {
        FaceData<double> faceWeight(*(MainApp::instance->mesh));
        for (int i = 0; i < 100; i++)
        {
            for (Face f : (MainApp::instance->mesh)->faces())
            {
                faceWeight[f] = 10 * (-remeshing::findBarycenter((MainApp::instance->geom), f).z + 1.5) + 0;
            }
            remeshing::smoothByFaceWeight(MainApp::instance->mesh, MainApp::instance->geom, faceWeight);
            //         remeshing::fixDelaunay(MainApp::instance->mesh, MainApp::instance->geom);
        }
        MainApp::instance->reregisterMesh();
    }
    if (ImGui::Button("Remesh"))
    {
        remeshing::remesh(MainApp::instance->mesh, MainApp::instance->geom, MainApp::instance->geomOrig);
        MainApp::instance->reregisterMesh();
    }
    ImGui::EndGroup();

    // testing stuff

    ImGui::Text("Testing stuff");

    ImGui::BeginGroup();
    ImGui::InputInt("partIndex", &partIndex);
    if (ImGui::Button("Test collapse edge"))
    {
        remeshing::testCollapseEdge(MainApp::instance->mesh, MainApp::instance->geom, partIndex);
        MainApp::instance->reregisterMesh();
    }
    if (ImGui::Button("Test stuff"))
    {
        remeshing::testStuff(MainApp::instance->mesh, MainApp::instance->geom, partIndex);
        //      MainApp::instance->mesh->validateConnectivity();
        //      MainApp::instance->mesh->compress();
        MainApp::instance->reregisterMesh();
    }

    if (ImGui::Button("Test stuff 2"))
    {
        remeshing::testStuff2(MainApp::instance->mesh, MainApp::instance->geom, MainApp::instance->geomOrig);
        MainApp::instance->reregisterMesh();
    }

    if (ImGui::Button("Show Vertex"))
    {
        remeshing::showEdge(MainApp::instance->mesh, MainApp::instance->geom, partIndex);
        MainApp::instance->reregisterMesh();
    }
    if (ImGui::Button("Validate"))
    {
        MainApp::instance->mesh->validateConnectivity();
    }

    if (ImGui::Button("Test vertex"))
    {
        remeshing::testVertex(MainApp::instance->mesh, MainApp::instance->geom, partIndex);
        //      MainApp::instance->mesh->validateConnectivity();
        //      MainApp::instance->mesh->compress();
        MainApp::instance->reregisterMesh();
    }
    if (ImGui::Button("Test edge"))
    {
        remeshing::testEdge(MainApp::instance->mesh, MainApp::instance->geom, partIndex);
        //      MainApp::instance->mesh->validateConnectivity();
        //      MainApp::instance->mesh->compress();
        MainApp::instance->reregisterMesh();
    }
    if (ImGui::Button("Test face"))
    {
        remeshing::testFace(MainApp::instance->mesh, MainApp::instance->geom, partIndex);
        //      MainApp::instance->mesh->validateConnectivity();
        //      MainApp::instance->mesh->compress();
        MainApp::instance->reregisterMesh();
    }
    ImGui::EndGroup();
}

struct MeshAndEnergy
{
    rsurfaces::TPEKernel *kernel;
    polyscope::SurfaceMesh *psMesh;
    rsurfaces::MeshPtr mesh;
    rsurfaces::GeomPtr geom;
    std::string meshName;
};

MeshAndEnergy initTPEOnMesh(std::string meshFile, double alpha, double beta)
{
    using namespace rsurfaces;
    std::cout << "Initializing tangent-point energy with (" << alpha << ", " << beta << ")" << std::endl;

    std::unique_ptr<HalfedgeMesh> u_mesh;
    std::unique_ptr<VertexPositionGeometry> u_geometry;
    // Load mesh
    std::tie(u_mesh, u_geometry) = loadMesh(meshFile);
    std::string mesh_name = polyscope::guessNiceNameFromPath(meshFile);

    // Register the mesh with polyscope
    polyscope::SurfaceMesh *psMesh = polyscope::registerSurfaceMesh(mesh_name,
                                                                    u_geometry->inputVertexPositions, u_mesh->getFaceVertexList(),
                                                                    polyscopePermutations(*u_mesh));

    MeshPtr meshShared = std::move(u_mesh);
    GeomPtr geomShared = std::move(u_geometry);

    geomShared->requireFaceNormals();
    geomShared->requireFaceAreas();
    geomShared->requireVertexNormals();
    geomShared->requireVertexDualAreas();
    geomShared->requireVertexGaussianCurvatures();

    TPEKernel *tpe = new rsurfaces::TPEKernel(meshShared, geomShared, alpha, beta);

    return MeshAndEnergy{tpe, psMesh, meshShared, geomShared, mesh_name};
}

rsurfaces::SurfaceFlow *setUpFlow(MeshAndEnergy &m, double theta, rsurfaces::scene::SceneData &scene, bool useCoulomb)
{
    using namespace rsurfaces;

    SurfaceEnergy *energy;

    if (useCoulomb)
    {
        energy = new CoulombEnergy(m.kernel, theta);
    }
    else if (theta <= 0)
    {
        std::cout << "Theta was zero (or negative); using exact all-pairs energy." << std::endl;
        energy = new AllPairsTPEnergy(m.kernel);
    }
    else
    {
        std::cout << "Using Barnes-Hut energy with theta = " << theta << "." << std::endl;
        BarnesHutTPEnergy6D *bh = new BarnesHutTPEnergy6D(m.kernel, theta);
        bh->disableNearField = scene.disableNearField;
        if (bh->disableNearField)
        {
            std::cout << "Near-field interactions are disabled." << std::endl;
        }
        energy = bh;
    }

    SurfaceFlow *flow = new SurfaceFlow(energy);
    bool kernelRemoved = false;
    flow->allowBarycenterShift = scene.allowBarycenterShift;
    // Set these up here, so that we can aggregate all vertex pins into the same constraint
    Constraints::VertexPinConstraint *pinC = 0;
    Constraints::VertexNormalConstraint *normC = 0;

    for (scene::ConstraintData &data : scene.constraints)
    {
        switch (data.type)
        {
        case scene::ConstraintType::Barycenter:
            kernelRemoved = true;
            flow->addSimpleConstraint<Constraints::BarycenterConstraint3X>(m.mesh, m.geom);
            break;
        case scene::ConstraintType::TotalArea:
            flow->addSchurConstraint<Constraints::TotalAreaConstraint>(m.mesh, m.geom, data.targetMultiplier, data.numIterations);
            break;
        case scene::ConstraintType::TotalVolume:
            flow->addSchurConstraint<Constraints::TotalVolumeConstraint>(m.mesh, m.geom, data.targetMultiplier, data.numIterations);
            break;

        case scene::ConstraintType::BoundaryPins:
        {
            if (!pinC)
            {
                pinC = flow->addSimpleConstraint<Constraints::VertexPinConstraint>(m.mesh, m.geom);
            }
            // Manually add all of the boundary vertex indices as pins
            std::vector<size_t> boundaryInds;
            VertexIndices inds = m.mesh->getVertexIndices();
            for (GCVertex v : m.mesh->vertices())
            {
                if (v.isBoundary())
                {
                    boundaryInds.push_back(inds[v]);
                }
            }
            pinC->pinVertices(m.mesh, m.geom, boundaryInds);
            kernelRemoved = true;
        }
        break;

        case scene::ConstraintType::VertexPins:
        {
            if (!pinC)
            {
                pinC = flow->addSimpleConstraint<Constraints::VertexPinConstraint>(m.mesh, m.geom);
            }
            // Add the specified vertices as pins
            pinC->pinVertices(m.mesh, m.geom, scene.vertexPins);
            // Clear the data vector so that we don't add anything twice
            scene.vertexPins.clear();
            kernelRemoved = true;
        }
        break;

        case scene::ConstraintType::BoundaryNormals:
        {
            if (!normC)
            {
                normC = flow->addSimpleConstraint<Constraints::VertexNormalConstraint>(m.mesh, m.geom);
            }
            // Manually add all of the boundary vertex indices as pins
            std::vector<size_t> boundaryInds;
            VertexIndices inds = m.mesh->getVertexIndices();
            for (GCVertex v : m.mesh->vertices())
            {
                if (v.isBoundary())
                {
                    boundaryInds.push_back(inds[v]);
                }
            }
            normC->pinVertices(m.mesh, m.geom, boundaryInds);
        }

        case scene::ConstraintType::VertexNormals:
        {
            if (!normC)
            {
                normC = flow->addSimpleConstraint<Constraints::VertexNormalConstraint>(m.mesh, m.geom);
            }
            // Add the specified vertices as pins
            normC->pinVertices(m.mesh, m.geom, scene.vertexNormals);
            // Clear the data vector so that we don't add anything twice
            scene.vertexNormals.clear();
        }
        break;

        default:
            std::cout << "  * Skipping unrecognized constraint type" << std::endl;
            break;
        }
    }

    if (!kernelRemoved)
    {
        // std::cout << "Auto-adding barycenter constraint to eliminate constant kernel of Laplacian" << std::endl;
        // flow->addSimpleConstraint<Constraints::BarycenterConstraint3X>(m.mesh, m.geom);
    }

    return flow;
}

rsurfaces::scene::SceneData defaultScene(std::string meshName)
{
    using namespace rsurfaces;
    using namespace rsurfaces::scene;
    SceneData data;
    data.meshName = meshName;
    data.alpha = 6;
    data.beta = 12;
    data.constraints = std::vector<ConstraintData>({ConstraintData{scene::ConstraintType::Barycenter, 1, 0},
                                                    ConstraintData{scene::ConstraintType::TotalArea, 1, 0},
                                                    ConstraintData{scene::ConstraintType::TotalVolume, 1, 0}});
    return data;
}

int main(int argc, char **argv)
{
    using namespace rsurfaces;

    // Configure the argument parser
    args::ArgumentParser parser("geometry-central & Polyscope example project");
    args::Positional<std::string> inputFilename(parser, "mesh", "A mesh file.");
    args::ValueFlag<double> thetaFlag(parser, "Theta", "Theta value for Barnes-Hut approximation; 0 means exact.", args::Matcher{'t', "theta"});
    args::ValueFlagList<std::string> obstacleFiles(parser, "obstacles", "Obstacles to add", {'o'});
    args::Flag autologFlag(parser, "autolog", "Automatically start the flow, log performance, and exit when done.", {"autolog"});
    args::Flag coulombFlag(parser, "coulomb", "Use a coulomb energy instead of the tangent-point energy.", {"coulomb"});

    int default_threads = omp_get_max_threads();
    std::cout << "OMP autodetected " << default_threads << " threads." << std::endl;
    omp_set_num_threads(default_threads / 2 + 2);

    polyscope::options::programName = "Repulsive Surfaces";
    polyscope::options::groundPlaneEnabled = false;

    std::cout << "Using Eigen version " << EIGEN_WORLD_VERSION << "." << EIGEN_MAJOR_VERSION << "." << EIGEN_MINOR_VERSION << std::endl;

    // Parse args
    try
    {
        parser.ParseCLI(argc, argv);
    }
    catch (args::Help)
    {
        std::cout << parser;
        return 0;
    }
    catch (args::ParseError e)
    {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }
    // Make sure a mesh name was given
    if (!inputFilename)
    {
        std::cerr << "Please specify a mesh file as argument" << std::endl;
        return EXIT_FAILURE;
    }

    double theta = 0.5;
    if (!thetaFlag)
    {
        std::cout << "Barnes-Hut theta value not specified; defaulting to theta = " << theta << std::endl;
    }
    else
    {
        theta = args::get(thetaFlag);
    }

    // Initialize polyscope
    polyscope::init();
    // Set the callback function
    polyscope::state::userCallback = customCallback;

    // Parse the input file, either as a scene file or as a mesh
    std::string inFile = args::get(inputFilename);
    scene::SceneData data;

    if (endsWith(inFile, ".txt") || endsWith(inFile, ".scene"))
    {
        std::cout << "Parsing " << inFile << " as scene file." << std::endl;
        data = scene::parseScene(inFile);
    }

    else if (endsWith(inFile, ".obj"))
    {
        std::cout << "Parsing " << inFile << " as OBJ mesh file." << std::endl;
        data = defaultScene(inFile);
    }

    else
    {
        throw std::runtime_error("Unknown file extension for " + inFile + ".");
    }

    bool useCoulomb = false;
    if (coulombFlag)
    {
        useCoulomb = true;
        std::cout << "Using Coulomb energy. (Note: Not expected to work well.)" << std::endl;
    }

    MeshAndEnergy m = initTPEOnMesh(data.meshName, data.alpha, data.beta);
    SurfaceFlow *flow = setUpFlow(m, theta, data, useCoulomb);
    flow->disableNearField = data.disableNearField;

    MainApp::instance = new MainApp(m.mesh, m.geom, flow, m.psMesh, m.meshName);
    MainApp::instance->bh_theta = theta;
    MainApp::instance->kernel = m.kernel;
    MainApp::instance->stepLimit = data.iterationLimit;
    MainApp::instance->realTimeLimit = data.realTimeLimit;
    MainApp::instance->methodChoice = data.defaultMethod;
    MainApp::instance->sceneData = data;

    if (autologFlag)
    {
        std::cout << "Autolog flag was used; starting flow automatically." << std::endl;
        MainApp::instance->exitWhenDone = true;
        MainApp::instance->logPerformance = true;
        run = true;
        std::ofstream outfile;
        outfile.open(data.performanceLogFile, std::ios_base::out);
        outfile.close();
    }

    for (scene::PotentialData &p : data.potentials)
    {
        MainApp::instance->AddPotential(p.type, p.weight);
    }
    for (scene::ObstacleData &obs : data.obstacles)
    {
        MainApp::instance->AddObstacle(obs.obstacleName, obs.weight, obs.recenter);
    }
    for (scene::ImplicitBarrierData &barrierData : data.implicitBarriers)
    {
        MainApp::instance->AddImplicitBarrier(barrierData);
    }

    MainApp::instance->updateMeshPositions();

    // Give control to the polyscope gui
    polyscope::show();

    return EXIT_SUCCESS;
}
