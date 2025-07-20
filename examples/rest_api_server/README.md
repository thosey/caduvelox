# REST API Server Example

A production-ready REST API server demonstrating CRUD operations, authentication, and file uploads.

## Features

- RESTful API design
- JWT authentication
- CRUD operations for users and items
- File upload/download
- CORS support
- Request logging
- Input validation

## API Endpoints

### Authentication
- `POST /api/auth/login` - Login with username/password
- `POST /api/auth/register` - Register new user

### Users (requires auth)
- `GET /api/users` - List all users
- `GET /api/users/:id` - Get user by ID
- `PUT /api/users/:id` - Update user
- `DELETE /api/users/:id` - Delete user

### Items
- `GET /api/items` - List all items
- `POST /api/items` - Create item
- `GET /api/items/:id` - Get item by ID
- `PUT /api/items/:id` - Update item
- `DELETE /api/items/:id` - Delete item

### Files
- `POST /api/files` - Upload file
- `GET /api/files/:filename` - Download file

## Building

```bash
# First, build the main caduvelox library
cd ../../build
cmake ..
make

# Then build this example
cd ../examples/rest_api_server
mkdir build && cd build
cmake ..
make
```

## Running

```bash
# Run the server
./rest_api_server

# The server listens on http://localhost:8080
```

## Testing

```bash
# Register a user
curl -X POST http://localhost:8080/api/auth/register \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'

# Login
curl -X POST http://localhost:8080/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'

# Create an item (save the token from login)
curl -X POST http://localhost:8080/api/items \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"Widget","price":19.99}'

# List items
curl http://localhost:8080/api/items

# Upload a file
curl -X POST http://localhost:8080/api/files \
  -F "file=@myfile.txt"
```

## Configuration

- **Port**: 8080
- **Data Directory**: `/tmp/api_data`
- **Max Upload Size**: Configurable in code

## Security

- JWT token validation
- Path traversal protection on file operations
- CORS headers for browser security
- Input validation on all endpoints
