from flask import Flask, render_template
import sys

app = Flask(__name__, static_url_path='', static_folder='static')

@app.route('/')
def root():
    return render_template('index.html')


if __name__ == '__main__':

	address = '127.0.0.1'
	port = '5566'

	if len(sys.argv) == 3:
		address = sys.argv[1]
		port = sys.argv[2]

	print("server running at ", address, port)

	app.run(debug=True, host=address, port=port)