#include "SegmenterCLP.h"

#include <iostream>

// CGAL includes
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/Face_filtered_graph.h>
#include <CGAL/boost/graph/copy_face_graph.h>
#include <CGAL/boost/graph/graph_traits_Surface_mesh.h>
#include <CGAL/mesh_segmentation.h>

// vtk includes
#include <vtkCell.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDoubleArray.h>
#include <vtkFieldData.h>
#include <vtkFloatArray.h>
#include <vtkIdList.h>
#include <vtkIdTypeArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataReader.h>
#include <vtkPolyDataWriter.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedIntArray.h>

// MRML includes
#include "vtkMRMLModelNode.h"
#include "vtkMRMLModelStorageNode.h"


// vtkMRMLModelNode to polygon mesh 
template <typename TM>
int pmesh_from_vtkMRMLModelNode(std::string filename, TM &tmesh)
{
  
  typedef typename boost::property_map<TM, CGAL::vertex_point_t>::type VPMap;
  typedef typename boost::property_map_value<TM, CGAL::vertex_point_t>::type Point_3;
  typedef typename boost::graph_traits<TM>::vertex_descriptor vertex_descriptor;

  VPMap vpmap = get(CGAL::vertex_point, tmesh);
  

  // read the polydata from slicer model
  vtkNew<vtkMRMLModelStorageNode> modelStorageNode;
  vtkNew<vtkMRMLModelNode> modelNode;
  modelStorageNode->SetFileName(filename.c_str());
  if (!modelStorageNode->ReadData(modelNode))
    {
      std::cerr << "Failed to read input model" << std::endl;
      return EXIT_FAILURE;
    }
  vtkSmartPointer<vtkPolyData> polydata = modelNode->GetPolyData();

  // get nb of points and cells
  vtkIdType nb_points = polydata->GetNumberOfPoints();
  vtkIdType nb_cells = polydata->GetNumberOfCells();

  //extract points
  std::vector<vertex_descriptor> vertex_map(nb_points);
  for (vtkIdType i = 0; i<nb_points; ++i)
    {
      double coords[3];
      polydata->GetPoint(i, coords);

      vertex_descriptor v = add_vertex(tmesh);
      put(vpmap, v, Point_3(coords[0], coords[1], coords[2]));
      vertex_map[i]=v;
    }

  //extract cells
  for (vtkIdType i = 0; i<nb_cells; ++i)
    {
      if(polydata->GetCellType(i) != 5
         && polydata->GetCellType(i) != 7
         && polydata->GetCellType(i) != 9) //only supported cells are triangles, quads and polygons
        continue;
      vtkCell* cell_ptr = polydata->GetCell(i);

      vtkIdType nb_vertices = cell_ptr->GetNumberOfPoints();
      if (nb_vertices < 3)
        return false;
      std::vector<vertex_descriptor> vr(nb_vertices);
      for (vtkIdType k=0; k<nb_vertices; ++k)
        vr[k]=vertex_map[cell_ptr->GetPointId(k)];

      CGAL::Euler::add_face(vr, tmesh);
    }

  return EXIT_SUCCESS;

}


int main( int argc, char * argv[] )
{
  
  PARSE_ARGS;
  
  typedef CGAL::Exact_predicates_inexact_constructions_kernel Kernel;
  typedef CGAL::Surface_mesh<Kernel::Point_3> SM;

  typedef boost::graph_traits<SM>::vertex_descriptor   vertex_descriptor;
  typedef boost::graph_traits<SM>::face_descriptor     face_descriptor;
  typedef boost::graph_traits<SM>::halfedge_descriptor halfedge_descriptor;
  
  SM mesh;
    
  // Convert to GCAL mesh
  pmesh_from_vtkMRMLModelNode(inputModel, mesh); // TODO: safe downcast
  
  typedef SM::Property_map<face_descriptor,double> Facet_double_map;
  Facet_double_map sdf_property_map;

  sdf_property_map =
    mesh.add_property_map<face_descriptor,double>("f:sdf").first;

  CGAL::sdf_values(mesh, sdf_property_map); //, 2.0/3.0*3.14, 25, true);

  // Create a property-map for segment-ids
  typedef SM::Property_map<face_descriptor, std::size_t> Facet_int_map;
  Facet_int_map segment_property_map =
    mesh.add_property_map<face_descriptor,std::size_t>("f:sid").first;

  // Segment the mesh: Any other scalar values can be used instead of
  // using SDF values computed using the CGAL function.
  std::cout << "Segmenting mesh for clusters=" << num_clusters
            << ",lambda=" << smoothing_lambda << std::endl;
  std::size_t num_segments =
    CGAL::segmentation_from_sdf_values(mesh, sdf_property_map,
                                       segment_property_map,
                                       (std::size_t)num_clusters,
                                       smoothing_lambda);
  std::cout << "Resulting in number of segments:"
            << num_segments << std::endl;

  // Export to vtk
  typedef typename boost::property_map<SM, CGAL::vertex_point_t>::const_type VPMap;
  typedef typename boost::property_map_value<SM, CGAL::vertex_point_t>::type Point_3;
  VPMap vpmap = get(CGAL::vertex_point, mesh);

  // Setup vtk arrays
  vtkSmartPointer<vtkPoints> points =
    vtkSmartPointer<vtkPoints>::New();

  vtkSmartPointer<vtkCellArray> cells =
    vtkSmartPointer<vtkCellArray>::New();
  
  vtkSmartPointer<vtkDoubleArray> scalars =
    vtkSmartPointer<vtkDoubleArray>::New();
  scalars->SetNumberOfComponents(1);
  scalars->SetName("SDF");

  vtkSmartPointer<vtkUnsignedIntArray> segIds =
    vtkSmartPointer<vtkUnsignedIntArray>::New();
  segIds->SetNumberOfComponents(1);
  segIds->SetName("SegmentId");

  // Populate
  std::map<vertex_descriptor, vtkIdType> Vids;
  vtkIdType inum = 0;
  for(vertex_descriptor v : vertices(mesh))
    {
      const Point_3& p = get(vpmap, v);
      points->InsertNextPoint(CGAL::to_double(p.x()),
                              CGAL::to_double(p.y()),
                              CGAL::to_double(p.z()));
      Vids[v] = inum++;
    }
  for(face_descriptor f : faces(mesh))
    {
      vtkIdList* cell = vtkIdList::New();
      for(halfedge_descriptor h :
            halfedges_around_face(halfedge(f, mesh), mesh))
        {
          cell->InsertNextId(Vids[target(h, mesh)]);
        }
      cells->InsertNextCell(cell);
      cell->Delete();

      // Add scalar and segmentation id
      scalars->InsertNextValue(get(sdf_property_map, f));
      segIds->InsertNextValue(get(segment_property_map, f));

    }
  
  // Create a polydata object and add everything to it
  vtkSmartPointer<vtkPolyData> polydata =
    vtkSmartPointer<vtkPolyData>::New();
  polydata->SetPoints(points);
  polydata->SetPolys(cells);
  polydata->GetCellData()->AddArray(scalars);
  polydata->GetCellData()->AddArray(segIds);
  
  // Create slicer output model
  vtkNew<vtkMRMLModelNode> outputModelNode;
  outputModelNode->SetAndObservePolyData(polydata);
  vtkNew<vtkMRMLModelStorageNode> outputModelStorageNode;
  outputModelStorageNode->SetFileName(outputModel.c_str());
  if (!outputModelStorageNode->WriteData(outputModelNode))
    {
      std::cerr << "Failed to write output model file " << outputModel << std::endl;
      return EXIT_FAILURE;
    }
  
  return EXIT_SUCCESS;
  
}
