#pragma once

#include "block_cluster_tree2.h"
#include "cluster_tree2.h"

namespace rsurfaces
{
    inline BlockClusterTree2 *CreateOptimizedBCT(MeshPtr &mesh, GeomPtr &geom, double p, double theta)
    {
        int nVertices = mesh->nVertices();
        int nFaces = mesh->nFaces();
        FaceIndices fInds = mesh->getFaceIndices();
        VertexIndices vInds = mesh->getVertexIndices();

        double athird = 1. / 3.;
        int nnz = 3 * nFaces;
        std::vector<int> outer(nFaces + 1);
        outer[nFaces] = 3 * nFaces;
        std::vector<int> inner(nnz);
        std::vector<double> values(3 * nFaces, 1. / 3.);

        std::vector<int> ordering(nFaces);
        std::vector<double> P_coords(3 * nFaces);
        std::vector<double> P_hull_coords(9 * nFaces);
        std::vector<double> P_data(7 * nFaces);

        int facecounter = 0;

        for (auto face : mesh->faces()) // when I have to use a "." to reference into members of face then there is a lot of copying going on
        {
            int i = fInds[face];

            ordering[i] = i; // unless we know anything better, let's use the identity permutation.
            outer[facecounter] = 3 * facecounter;
            facecounter++;

            GCHalfedge he = face.halfedge();

            int i0 = vInds[he.vertex()];
            int i1 = vInds[he.next().vertex()];
            int i2 = vInds[he.next().next().vertex()];
            Vector3 p1 = geom->inputVertexPositions[i0];
            Vector3 p2 = geom->inputVertexPositions[i1];
            Vector3 p3 = geom->inputVertexPositions[i2];

            P_coords[3 * i + 0] = athird * (p1.x + p2.x + p3.x);
            P_coords[3 * i + 1] = athird * (p1.y + p2.y + p3.y);
            P_coords[3 * i + 2] = athird * (p1.z + p2.z + p3.z);

            P_data[7 * i + 0] = geom->faceAreas[face];
            P_data[7 * i + 1] = P_coords[3 * i + 0];
            P_data[7 * i + 2] = P_coords[3 * i + 1];
            P_data[7 * i + 3] = P_coords[3 * i + 2];
            P_data[7 * i + 4] = geom->faceNormals[face].x;
            P_data[7 * i + 5] = geom->faceNormals[face].y;
            P_data[7 * i + 6] = geom->faceNormals[face].z;

            P_hull_coords[9 * i + 0] = p1.x;
            P_hull_coords[9 * i + 1] = p1.y;
            P_hull_coords[9 * i + 2] = p1.z;
            P_hull_coords[9 * i + 3] = p2.x;
            P_hull_coords[9 * i + 4] = p2.y;
            P_hull_coords[9 * i + 5] = p2.z;
            P_hull_coords[9 * i + 6] = p3.x;
            P_hull_coords[9 * i + 7] = p3.y;
            P_hull_coords[9 * i + 8] = p3.z;

            inner[3 * i + 0] = i0;
            inner[3 * i + 1] = i1;
            inner[3 * i + 2] = i2;

            std::sort(inner.begin() + 3 * i, inner.begin() + 3 * (i + 1));
        }

        EigenMatrixCSR DiffOp = Hs::BuildDfOperator(mesh, geom); // This is a sparse matrix in CSC!!! format.
        DiffOp.makeCompressed();
        EigenMatrixCSR AvOp = Eigen::Map<EigenMatrixCSR>(nFaces, nVertices, nnz, &outer[0], &inner[0], &values[0]); // This is a sparse matrix in CSR format.
        AvOp.makeCompressed();
        // create a cluster tree
        int split_threashold = 8;
        ClusterTree2 *bvh = new ClusterTree2(
            &P_coords[0],      // coordinates used for clustering
            nFaces,            // number of primitives
            3,                 // dimension of ambient space
            &P_hull_coords[0], // coordinates of the convex hull of each mesh element
            3,                 // number of points in the convex hull of each mesh element (3 for triangle meshes, 2 for polylines)
            &P_data[0],        // area, barycenter, and normal of mesh element
            7,                 // number of dofs of P_data per mesh element; it is 7 for plylines and triangle meshes in 3D.
            3 * 3,             // some estimate for the buffer size per vertex and cluster (usually the square of the dimension of the embedding space
            &ordering[0],      // some ordering of triangles
            split_threashold,  // create clusters only with at most this many mesh elements in it
            DiffOp,            // the first-order differential operator belonging to the hi order term of the metric
            AvOp               // the zeroth-order differential operator belonging to the lo order term of the metric
        );

        mreal alpha = p;
        mreal beta = 2 * p;
        BlockClusterTree2 *bct = new BlockClusterTree2(
            bvh,   // gets handed two pointers to instances of ClusterTree2
            bvh,   // no problem with handing the same pointer twice; this is actually intended
            alpha, // first parameter of the energy (for the numerator)
            beta,  // second parameter of the energy (for the denominator)
            theta  // separation parameter; different gauge for thetas as before are the block clustering is performed slightly differently from before
        );

        return bct;
    }
} // namespace rsurfaces