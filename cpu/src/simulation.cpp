#include "simulation.h"

#include "distanceconstraint.h"
#include "totalshapeconstraint.h"
#include "boundaryconstraint.h"
#include "contactconstraint.h"
#include "rigidcontactconstraint.h"
#include "totalfluidconstraint.h"
#include "gasconstraint.h"

Simulation::Simulation()
{
    init(FRICTION_TEST);
    debug = true;
}

Simulation::~Simulation()
{
    clear();
}

void Simulation::clear() {
    for(int i = m_particles.size()-1; i >= 0; i--) {
        Particle *p = m_particles.at(i);
        m_particles.removeAt(i);
        delete(p);
    }
    for(int i = m_bodies.size()-1; i >= 0; i--) {
        Body *b = m_bodies.at(i);
        m_bodies.removeAt(i);
        delete(b);
    }
    for (int i = 0; i < NUM_CONSTRAINT_GROUPS; i++) {
        if(m_globalConstraints.contains((ConstraintGroup) i)) {
            QList<Constraint *> group = m_globalConstraints[(ConstraintGroup) i];
            for (int j = group.size()-1; j >=0; j--) {
                Constraint *c = group.at(j);
                for (int k = 0; k < NUM_CONSTRAINT_GROUPS; k++) {
                    if(m_globalConstraints.contains((ConstraintGroup) k)) {
                        m_globalConstraints[(ConstraintGroup) k].removeAll(c);
                    }
                }
                delete(c);
            }
        }
    }
}

void Simulation::init(SimulationType type)
{
    this->clear();

    // Default gravity value
    m_gravity = glm::dvec2(0,-9.8);

    switch (type) {
    case FRICTION_TEST:
        initFriction(); break;
    case GRANULAR_TEST:
        initGranular(); break;
    case STACKS_TEST:
        initBoxes(); break;
    case WALL_TEST:
        initWall(); break;
    case PENDULUM_TEST:
        initPendulum(); break;
    case FLUID_TEST:
        initFluid(); break;
    case FLUID_SOLID_TEST:
        initFluidSolid(); break;
    case GAS_TEST:
        initGas(); break;
    default:
        initBoxes(); break;
    }

    // Set up the M^-1 matrix
    m_standardSolver.setupM(&m_particles);
}

// (#) in the main simulation loop refer to lines from the main loop in the paper
void Simulation::tick(double seconds)
{
    QHash<ConstraintGroup, QList<Constraint *> > constraints;

    // Add all rigid body shape constraints
    for (int i = 0; i < m_bodies.size(); i++) {
        Body *b = m_bodies[i];
        if (TotalShapeConstraint *c = dynamic_cast<TotalShapeConstraint *>(b->shape)) {
            constraints[SHAPE].append(c);
        } else {
            cout << "Rigid body's attached constraint was not a shape constraint." << endl;
            exit(1);
        }
    }

    // Add all other global constraints
    for (int i = 0; i < m_globalConstraints.size(); i++) {
        QList<Constraint *> group = m_globalConstraints[(ConstraintGroup) i];
        for (int j = 0; j < group.size(); j++) {
            constraints[(ConstraintGroup) i].append(group.at(j));
        }
    }

    // (1) For all particles
    for (int i = 0; i < m_particles.size(); i++) {
        Particle *p = m_particles[i];

        // (2) Apply forces (gravity)
        glm::dvec2 myGravity = m_gravity;
        if(p->ph == GAS) myGravity *= ALPHA;
        p->v = p->v + seconds * myGravity;

        // (3) Predict positions
        p->ep = p->guess(seconds);

        // (4) Apply mass scaling (used by certain constraints)
        p->scaleMass();
    }
    // (5) End for

    m_contactSolver.setupM(&m_particles, true);

    // (6) For all particles
    for (int i = 0; i < m_particles.size(); i++) {
        Particle *p = m_particles[i];

        // (7) Find neighboring particles and solid contacts, naive solution
        for (int j = i + 1; j < m_particles.size(); j++) {
            Particle *p2 = m_particles[j];

            // Skip collision between two immovables
            if (p->imass == 0 && p2->imass == 0) {
                continue;

            // Skip collisions betwee particles in the same rigid body
            } else if (p->ph == SOLID && p2->ph == SOLID && p->bod == p2->bod && p->bod != -1) {
                continue;
            } else {

                // Collision happens when circles overlap
                double dist = glm::distance(p->ep, p2->ep);
                if (dist < PARTICLE_DIAM - EPSILON) {

                    // Rigid contact constraints (which include friction) apply to solid-solid contact
                    if (p->ph == SOLID && p2->ph == SOLID) {
                        constraints[CONTACT].append(new RigidContactConstraint(i, j, &m_bodies));
#ifdef USE_STABILIZATION
                        constraints[STABILIZATION].append(new RigidContactConstraint(i, j, &m_bodies));
#endif
                    // Regular contact constraints (which have no friction) apply to other solid-other contact
                    } else if (p->ph == SOLID || p2->ph == SOLID) {
                        constraints[CONTACT].append(new ContactConstraint(i, j));
                    }
                }
            }
        }

        // (8) Find solid boundary contacts
        if (p->ep.x < m_xBoundaries.x + PARTICLE_RAD) {
            constraints[CONTACT].append(new BoundaryConstraint(i, m_xBoundaries.x, true, true));
#ifdef USE_STABILIZATION
            constraints[STABILIZATION].append(new BoundaryConstraint(i, m_xBoundaries.x, true, true, true));
#endif
        } else if (p->ep.x > m_xBoundaries.y - PARTICLE_RAD) {
            constraints[CONTACT].append(new BoundaryConstraint(i, m_xBoundaries.y, true, false));
#ifdef USE_STABILIZATION
            constraints[STABILIZATION].append(new BoundaryConstraint(i, m_xBoundaries.y, true, false, true));
#endif
        }

        if (p->ep.y < m_yBoundaries.x + PARTICLE_RAD) {
            constraints[CONTACT].append(new BoundaryConstraint(i, m_yBoundaries.x, false, true));
#ifdef USE_STABILIZATION
            constraints[STABILIZATION].append(new BoundaryConstraint(i, m_yBoundaries.x, false, true, true));
#endif
        } else if (p->ep.y > m_yBoundaries.y - PARTICLE_RAD) {
            constraints[CONTACT].append(new BoundaryConstraint(i, m_yBoundaries.y, false, false));
#ifdef USE_STABILIZATION
            constraints[STABILIZATION].append(new BoundaryConstraint(i, m_yBoundaries.y, false, false, true));
#endif
        }
    }
    // (9) End for

    m_contactSolver.setupSizes(m_particles.size(), &constraints[STABILIZATION]);

#ifdef USE_STABILIZATION

    // (10) For stabilization iterations
    for (int i = 0; i < STABILIZATION_ITERATIONS; i++) {

#ifdef ITERATIVE
        // (11, 12, 13, 14) Solve contact constraints and update p, ep, and n
        for (int k = 0; k < constraints[STABILIZATION].size(); k++) {
            constraints[STABILIZATION].at(k)->project(&m_particles);
        }
#else
        // (11, 12, 13, 14) Solve contact constraints and update p, ep, and n
        if (constraints[STABILIZATION].size() > 0) {
            m_contactSolver.solveAndUpdate(&m_particles, &constraints[STABILIZATION], true);
        } else {
            break;
        }
#endif

    }
    // (15) End for

#endif

#ifdef ITERATIVE

    // (16) For solver iterations
    for (int i = 0; i < SOLVER_ITERATIONS; i++) {

        // (17) For constraint group
        for (int j = 0; j < (int) NUM_CONSTRAINT_GROUPS; j++) {
            ConstraintGroup g = (ConstraintGroup) j;

            // Skip the stabilization constraints
            if (g == STABILIZATION) {
                continue;
            }

            //  (18, 19, 20) Solve constraints in g and update ep and n
            for (int k = 0; k < constraints[g].size(); k++) {
                constraints[g].at(k)->project(&m_particles);
            }
        }
    }

#else

    m_standardSolver.setupSizes(m_particles.size(), &constraints[STANDARD]);
    m_contactSolver.setupSizes(m_particles.size(), &constraints[CONTACT]);

    // (16) For solver iterations
    for (int i = 0; i < SOLVER_ITERATIONS; i++) {

        // (17, 18, 19, 20) for constraint group, solve constraints and update ep
        if (constraints[CONTACT].size() > 0) {
            m_contactSolver.solveAndUpdate(&m_particles, &constraints[CONTACT]);
        }

        if (constraints[STANDARD].size() > 0) {
            m_standardSolver.solveAndUpdate(&m_particles, &constraints[STANDARD]);
        }

        if (constraints[SHAPE].size() > 0) {
            for (int j = 0; j < constraints[SHAPE].size(); j++) {
                constraints[SHAPE][j]->project(&m_particles);
            }
        }
        // (21) End for
    }
    // (22) End for
#endif

    // (23) For all particles
    for (int i = 0; i < m_particles.size(); i++) {
        Particle *p = m_particles[i];

        // (24) Update velocities
        p->v = (p->ep - p->p) / seconds;

        // (25, 26) Advect diffuse particles, apply internal forces
        /// TODO

        // (27) Update positions or apply sleeping
        p->confirmGuess();
    }
    // (28) End for

    // Delete temporary conact constraints
    for(int i = constraints[CONTACT].size()-1; i >= 0; i--) {
        Constraint *c = constraints[CONTACT].at(i);
        constraints[CONTACT].removeAt(i);
        delete(c);
    }
    for(int i = constraints[STABILIZATION].size()-1; i >= 0; i--) {
        Constraint *c = constraints[STABILIZATION].at(i);
        constraints[STABILIZATION].removeAt(i);
        delete(c);
    }
}

Body *Simulation::createRigidBody(QList<Particle *> *verts, QList<SDFData> *sdfData)
{
    if(verts->size() <= 1) {
        cout << "Rigid bodies must be at least 2 points." << endl;
        exit(1);
    }

    // Compute the total mass, add all the particles to the system and the body
    Body *body = new Body(); int offset = m_particles.size(), bodyIdx = m_bodies.size();
    double totalMass = 0.0;
    for (int i = 0; i < verts->size(); i++) {
        Particle *p = verts->at(i);
        p->bod = bodyIdx;
        p->ph = SOLID;

        if (p->imass == 0.0) {
            cout << "A rigid body cannot have a point of infinite mass." << endl;
            exit(1);
        }

        totalMass += (1.0 / p->imass);

        m_particles.append(p);
        body->particles.append(i + offset);
        body->sdf[i + offset] = sdfData->at(i);
    }

    // Update the body's global properties, including initial r_i vectors
    body->imass = 1.0 / totalMass;
    body->updateCOM(&m_particles, false);
    body->computeRs(&m_particles);
    body->shape = new TotalShapeConstraint(body);

    m_bodies.append(body);
    return body;
}

void Simulation::createGas(QList<Particle *> *verts, double density)
{
    int offset = m_particles.size();
    int bod = 100 * frand();
    QList<int> indices;
    for (int i = 0; i < verts->size(); i++) {
        Particle *p = verts->at(i);
        p->ph = GAS;
        p->bod = bod;

        if (p->imass == 0.0) {
            cout << "A fluid cannot have a point of infinite mass." << endl;
            exit(1);
        }

        m_particles.append(p);
        indices.append(offset + i);
    }
    m_globalConstraints[STANDARD].append(new GasConstraint(density, &indices));
}

void Simulation::createFluid(QList<Particle *> *verts, double density)
{
    int offset = m_particles.size();
    int bod = 100 * frand();
    QList<int> indices;
    for (int i = 0; i < verts->size(); i++) {
        Particle *p = verts->at(i);
        p->ph = FLUID;
        p->bod = bod;

        if (p->imass == 0.0) {
            cout << "A fluid cannot have a point of infinite mass." << endl;
            exit(1);
        }

        m_particles.append(p);
        indices.append(offset + i);
    }
    m_globalConstraints[STANDARD].append(new TotalFluidConstraint(density, &indices));
}

void Simulation::draw()
{
    drawGrid();
    if (debug) {
        drawParticles();
    }
    drawBodies();
    drawGlobals();

    glColor3f(1,1,1);
    glPointSize(5);
    glBegin(GL_POINTS);
    glVertex2f(m_point.x, m_point.y);
    glEnd();
}

void Simulation::resize(const glm::ivec2 &dim)
{
    m_dimensions = dim;
}

void Simulation::drawGrid()
{
    glColor3f(.2,.2,.2);
    glBegin(GL_LINES);

    for (int x = -m_dimensions.x; x <= m_dimensions.x; x++) {
        glVertex2f(x, -m_dimensions.y);
        glVertex2f(x, m_dimensions.y);
    }
    for (int y = -m_dimensions.y; y <= m_dimensions.y; y++) {
        glVertex2f(-m_dimensions.y, y);
        glVertex2f(m_dimensions.y, y);
    }

    glColor3f(1,1,1);

    glVertex2f(-m_dimensions.x, 0);
    glVertex2f(m_dimensions.x, 0);
    glVertex2f(0, -m_dimensions.y);
    glVertex2f(0, m_dimensions.y);

    glEnd();

    glLineWidth(3);
    glBegin(GL_LINES);
    glVertex2f(m_xBoundaries.x, m_yBoundaries.x);
    glVertex2f(m_xBoundaries.x, m_yBoundaries.y);

    glVertex2f(m_xBoundaries.y, m_yBoundaries.x);
    glVertex2f(m_xBoundaries.y, m_yBoundaries.y);

    glVertex2f(m_xBoundaries.x, m_yBoundaries.x);
    glVertex2f(m_xBoundaries.y, m_yBoundaries.x);

    glVertex2f(m_xBoundaries.x, m_yBoundaries.y);
    glVertex2f(m_xBoundaries.y, m_yBoundaries.y);
    glEnd();
    glLineWidth(1);
}

void Simulation::drawParticles()
{
    for (int i = 0; i < m_particles.size(); i++) {
        const Particle *p = m_particles[i];

        if (p->imass == 0.f) {
            glColor3f(1,0,0);
        } else if (p->ph == FLUID || p->ph == GAS){
            glColor3f(0,p->bod / 100., 1-p->bod / 100.);
        } else if (p->ph == SOLID) {
            glColor3f(.8,.4,.3);
        } else {
            glColor3f(0,0,1);
        }

        glPushMatrix();
        glTranslatef(p->p.x, p->p.y, 0);
        glScalef(PARTICLE_RAD, PARTICLE_RAD, 0);
        drawCircle();
        glPopMatrix();
    }

    glEnd();
}

void Simulation::drawBodies()
{
    for (int i = 0; i < m_bodies.size(); i++) {
        Body *b = m_bodies[i];
        if (debug) {
            b->shape->draw(&m_particles);
        } else {
            b->draw(&m_particles);
        }
    }
}

void Simulation::drawGlobals()
{
    for (int i = 0; i < m_globalConstraints.size(); i++) {
        for (int j = 0; j < m_globalConstraints[(ConstraintGroup) i].size(); j++) {
            m_globalConstraints[(ConstraintGroup)i ][j]->draw(&m_particles);
        }
    }
}

void Simulation::drawCircle()
{
    glBegin(GL_TRIANGLE_FAN);

    glVertex2f(0,0);
    for (int f = 0; f <= 32; f++) {
        double a = f * M_PI / 16.f;
        glVertex2f(sin(a), cos(a));
    }

    glEnd();
}

void Simulation::initFriction()
{
    m_xBoundaries = glm::dvec2(-20,20);
    m_yBoundaries = glm::dvec2(0,1000000);

    double root2 = sqrt(2);
    QList<Particle *> vertices;
    QList<SDFData> data;
    data.append(SDFData(glm::normalize(glm::dvec2(-1,-1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(-1,1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(0,-1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(0,1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(1,-1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(1,1)), PARTICLE_RAD * root2));

    glm::ivec2 dim = glm::ivec2(3,2);
    for (int x = 0; x < dim.x; x++) {
        double xVal = PARTICLE_DIAM * ((x % dim.x) - dim.x / 2);
        for (int y = 0; y < dim.y; y++) {
            double yVal = (dim.y + (y % dim.y) + 1) * PARTICLE_DIAM;
            Particle *part =new Particle(glm::dvec2(xVal, yVal), (x == 0 && y == 0 ? 1 : 1.));
            part->v.x = 5;
            part->kFriction = .01;
            part->sFriction = .1;
            vertices.append(part);
        }
    }
    Body *body = createRigidBody(&vertices, &data);
}

void Simulation::initGranular()
{
    m_xBoundaries = glm::dvec2(-100,100);
    m_yBoundaries = glm::dvec2(-5, 1000);
    m_gravity = glm::dvec2(0,-9.8);

    for (int i = -10; i <= 10; i++) {
        for (int j = 0; j < 40; j++) {
            glm::dvec2 pos = glm::dvec2(i * (PARTICLE_DIAM + EPSILON), j * PARTICLE_DIAM + PARTICLE_RAD + m_yBoundaries.x);
            Particle *part= new Particle(pos, 1, SOLID);
            part->sFriction = .1;
            part->kFriction = .02;
            m_particles.append(part);
        }
    }

    Particle *jerk = new Particle(glm::dvec2(-5.51, 4), 100.f, SOLID);
    jerk->v.x = 10;
    m_particles.append(jerk);
}

void Simulation::initBoxes()
{
    m_xBoundaries = glm::dvec2(-20,20);
    m_yBoundaries = glm::dvec2(0,1000000);

    int numBoxes = 8, numColumns = 2;
    double root2 = sqrt(2);
    QList<Particle *> vertices;
    QList<SDFData> data;
    data.append(SDFData(glm::normalize(glm::dvec2(-1,-1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(-1,1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(0,-1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(0,1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(1,-1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(1,1)), PARTICLE_RAD * root2));

    for (int j = -numColumns; j <= numColumns; j++) {
        glm::ivec2 dim = glm::ivec2(3,2);
        for (int i = numBoxes - 1; i >= 0; i--) {
            for (int x = 0; x < dim.x; x++) {
                double xVal = j * 4 + PARTICLE_DIAM * ((x % dim.x) - dim.x / 2);
                for (int y = 0; y < dim.y; y++) {
                    double yVal = ((2 * i + 1) * dim.y + (y % dim.y) + 1) * PARTICLE_DIAM;
                    vertices.append(new Particle(glm::dvec2(xVal, yVal), (x == 0 && y == 0 ? 1 : 1.)));
                }
            }
            Body *body = createRigidBody(&vertices, &data);
            vertices.clear();
        }
    }
}

void Simulation::initWall()
{
    m_xBoundaries = glm::dvec2(-20,20);
    m_yBoundaries = glm::dvec2(0,1000000);

    glm::dvec2 dim = glm::dvec2(6,2);
    int height = 5, width = 2;
    double root2 = sqrt(2);
    QList<Particle *> vertices;
    QList<SDFData> data;
    data.append(SDFData(glm::normalize(glm::dvec2(-1,-1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(-1,1)), PARTICLE_RAD * root2));

    for (int i = 0; i < dim.x - 2; i++) {
        data.append(SDFData(glm::normalize(glm::dvec2(0,-1)), PARTICLE_RAD));
        data.append(SDFData(glm::normalize(glm::dvec2(0,1)), PARTICLE_RAD));
    }

    data.append(SDFData(glm::normalize(glm::dvec2(1,-1)), PARTICLE_RAD * root2));
    data.append(SDFData(glm::normalize(glm::dvec2(1,1)), PARTICLE_RAD * root2));

    for (int j = -width; j <= width; j++) {
        for (int i = height - 1; i >= 0; i--) {
            for (int x = 0; x < dim.x; x++) {
                double num = (i % 2 == 0 ? 3 : -1);
                double xVal = j * (EPSILON + dim.x / 2.) + PARTICLE_DIAM * (x % (int)dim.x) - num * PARTICLE_RAD;
                for (int y = 0; y < dim.y; y++) {
                    double yVal = (i * dim.y + (y % (int)dim.y) + EPSILON) * PARTICLE_DIAM + PARTICLE_RAD;
                    Particle *part = new Particle(glm::dvec2(xVal, yVal), 1.);
                    part->sFriction = 1;
                    part->kFriction = .09;
                    vertices.append(part);
                }
            }
            Body *body = createRigidBody(&vertices, &data);
            vertices.clear();
        }
    }
}

void Simulation::initPendulum()
{
    m_xBoundaries = glm::dvec2(-10,10);
    m_yBoundaries = glm::dvec2(0,1000000);

    int chainLength = 3;
    m_particles.append(new Particle(glm::dvec2(0, chainLength * 3 + 6) * PARTICLE_DIAM + glm::dvec2(0,2), 0, SOLID));

    QList<SDFData> data;
    data.append(SDFData(glm::normalize(glm::dvec2(-1,-1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(-1,1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(0,-1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(0,1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(1,-1)), PARTICLE_RAD));
    data.append(SDFData(glm::normalize(glm::dvec2(1,1)), PARTICLE_RAD));

    QList<Particle *> vertices;
    double xs[6] = {-1,-1,0,0,1,1};

    for (int i = chainLength; i >= 0; i--) {
        for (int j = 0; j < 6; j++) {
            double y = ((i + 1) * 3 + (j % 2)) * PARTICLE_DIAM + 2;
            vertices.append(new Particle(glm::dvec2(xs[j] * PARTICLE_DIAM, y), 1.));
        }
        Body *body = createRigidBody(&vertices, &data);
        vertices.clear();

        if (i < chainLength) {
            int basePrev = 1 + (chainLength - i - 1) * 6, baseCur = basePrev + 6;
            m_globalConstraints[STANDARD].append(new DistanceConstraint(baseCur + 1, basePrev, &m_particles));
            m_globalConstraints[STANDARD].append(new DistanceConstraint(baseCur + 5, basePrev + 4, &m_particles));
        }
    }

    m_globalConstraints[STANDARD].append(new DistanceConstraint(0, 4, &m_particles));
}

void Simulation::initFluid()
{
    double scale = 4., delta = .7;
    m_gravity = glm::dvec2(0,-9.8);
    m_xBoundaries = glm::dvec2(-2 * scale,2 * scale);
    m_yBoundaries = glm::dvec2(-2 * scale, 10 * scale);
    QList<Particle *> particles;

    double num = 2.;
    for (int d = 0; d < num; d++) {
        double start = -2 * scale + 4 * scale * (d / num);
        for(double x = start; x < start + (4 * scale / num); x += delta) {
            for(double y = -2 * scale; y < scale; y += delta) {
                particles.append(new Particle(glm::dvec2(x,y) + .2 * glm::dvec2(frand() - .5, frand() - .5), 1));
            }
        }
        createFluid(&particles, 1 + 1.5 * d);
        particles.clear();
    }

//    for (int x = -1; x <=1; x++) {
//        for (int y = 1; y <= 3; y++) {
//            particles.append(new Particle(glm::dvec2(x,y), 1., LIQUID));
//        }
//    }
//    createTotalFluid(&particles, 1.5);
}

void Simulation::initFluidSolid()
{
    double scale = 5., delta = .7;
    m_gravity = glm::dvec2(0, -9.8);
    m_xBoundaries = glm::dvec2(-2 * scale,2 * scale);
    m_yBoundaries = glm::dvec2(-2 * scale, 10 * scale);
    QList<Particle *> particles;

    double num = 1.;
    for (int d = 0; d < num; d++) {
        double start = -2 * scale + 4 * scale * (d / num);
        for(double x = start; x < start + (4 * scale / num); x += delta) {
            for(double y = -2 * scale; y < 2 * scale; y += delta) {
                particles.append(new Particle(glm::dvec2(x,y) + .2 * glm::dvec2(frand() - .5, frand() - .5), 1));
            }
        }
        createFluid(&particles, 1. + .75 * (d + 1));
        particles.clear();
    }

    if(true) {
        particles.clear();
        QList<SDFData> data;
        double root2 = sqrt(2);
        glm::ivec2 dim = glm::ivec2(5,2);
        data.append(SDFData(glm::normalize(glm::dvec2(-1,-1)), PARTICLE_RAD * root2));
        data.append(SDFData(glm::normalize(glm::dvec2(-1,1)), PARTICLE_RAD * root2));
        for (int i = 0; i < dim.x - 2; i++) {
            data.append(SDFData(glm::normalize(glm::dvec2(0,-1)), PARTICLE_RAD));
            data.append(SDFData(glm::normalize(glm::dvec2(0,1)), PARTICLE_RAD));
        }
        data.append(SDFData(glm::normalize(glm::dvec2(1,-1)), PARTICLE_RAD * root2));
        data.append(SDFData(glm::normalize(glm::dvec2(1,1)), PARTICLE_RAD * root2));

        for (int x = 0; x < dim.x; x++) {
            double xVal = PARTICLE_DIAM * ((x % dim.x) - dim.x / 2);
            for (int y = 0; y < dim.y; y++) {
                double yVal = (dim.y + (y % dim.y) + 1) * PARTICLE_DIAM;
                particles.append(new Particle(glm::dvec2(xVal-3, 15+yVal), .5));
            }
        }
        Body *body = createRigidBody(&particles, &data);
    }

    if(true) {
        particles.clear();
        QList<SDFData> data;
        double root2 = sqrt(2);
        glm::ivec2 dim = glm::ivec2(5,2);
        data.append(SDFData(glm::normalize(glm::dvec2(-1,-1)), PARTICLE_RAD * root2));
        data.append(SDFData(glm::normalize(glm::dvec2(-1,1)), PARTICLE_RAD * root2));
        for (int i = 0; i < dim.x - 2; i++) {
            data.append(SDFData(glm::normalize(glm::dvec2(0,-1)), PARTICLE_RAD));
            data.append(SDFData(glm::normalize(glm::dvec2(0,1)), PARTICLE_RAD));
        }
        data.append(SDFData(glm::normalize(glm::dvec2(1,-1)), PARTICLE_RAD * root2));
        data.append(SDFData(glm::normalize(glm::dvec2(1,1)), PARTICLE_RAD * root2));

        for (int x = 0; x < dim.x; x++) {
            double xVal = PARTICLE_DIAM * ((x % dim.x) - dim.x / 2);
            for (int y = 0; y < dim.y; y++) {
                double yVal = (dim.y + (y % dim.y) + 1) * PARTICLE_DIAM;
                particles.append(new Particle(glm::dvec2(xVal+3, 15+yVal), .2));
            }
        }
        Body *body = createRigidBody(&particles, &data);
    }
}


void Simulation::initGas()
{
    double scale = 2., delta = .7;
    m_gravity = glm::dvec2(0, -9.8);
    m_xBoundaries = glm::dvec2(-2  * scale,2 * scale);
    m_yBoundaries = glm::dvec2(-2  * scale, 10 * scale);
    QList<Particle *> particles;

    double num = 2.;
    for (int d = 0; d < num; d++) {
        double start = -2 * scale + 4 * scale * (d / num);
        for(double x = start; x < start + (4 * scale / num); x += delta) {
            for(double y = -2 * scale; y < 2 * scale; y += delta) {
                particles.append(new Particle(glm::dvec2(x,y) + .2 * glm::dvec2(frand() - .5, frand() - .5), 1));
            }
        }
        createGas(&particles, .75 + 3*(d));
        particles.clear();
    }

    scale = 3;
    for (int d = 0; d < num; d++) {
        double start = -2 * scale + 4 * scale * (d / num);
        for(double x = start; x < start + (4 * scale / num); x += delta) {
            for(double y = -2 * scale; y < 2 * scale; y += delta) {
                particles.append(new Particle(glm::dvec2(x,y+10) + .2 * glm::dvec2(frand() - .5, frand() - .5), 1));
            }
        }
        createFluid(&particles, 4. + .75 * (d + 1));
        particles.clear();
    }

}

int Simulation::getNumParticles()
{
    return m_particles.size();
}

double Simulation::getKineticEnergy()
{
    double energy = 0;
    for (int i = 0; i < m_particles.size(); i++) {
        Particle *p = m_particles[i];
        if (p->imass != 0.) {
            energy += .5 * glm::dot(p->v, p->v) / p->imass;
        }
    }
    return energy;
}

void Simulation::mousePressed(const glm::dvec2 &p)
{
    for (int i = 0; i < m_particles.size(); i++) {
        Particle *part = m_particles.at(i);

        glm::dvec2 to = glm::normalize(p - part->p);
        part->v += 7. * to;
    }
    m_point = p;
}